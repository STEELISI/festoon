#include <getopt.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_kni.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_per_lcore.h>
#include <rte_string_fns.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "params.h"
#include "rte_top.h"

static struct kni_port_params *kni_port_params_array[RTE_MAX_ETHPORTS];

/* Options for configuring ethernet port */
rte_eth_txmode tx_mode;
rte_eth_conf port_conf;

/* Mempool for mbufs */
static struct rte_mempool *pktmbuf_pool = NULL;

/* Mask of enabled ports */
static uint32_t ports_mask = 0;
/* Ports set in promiscuous mode off by default. */
static int promiscuous_on = 0;
/* Monitor link status continually. off by default. */
static int monitor_links;

/* kni device statistics array */
static struct kni_interface_stats kni_stats[RTE_MAX_ETHPORTS];

static int kni_change_mtu(uint16_t port_id, unsigned int new_mtu);

static int kni_config_network_interface(uint16_t port_id, uint8_t if_up);

static int kni_config_mac_address(uint16_t port_id, uint8_t mac_addr[]);

static rte_atomic32_t kni_stop = RTE_ATOMIC32_INIT(0);
static rte_atomic32_t kni_pause = RTE_ATOMIC32_INIT(0);

struct rte_ring *worker_tx_ring;
struct rte_ring *worker_rx_ring;

/* Print out statistics on packets handled */
static void print_stats(void) {
  uint16_t i;

  printf(
      "\n**KNI example application statistics**\n"
      "======  ==============  ============  ============  ============  "
      "============\n"
      " Port    Lcore(RX/TX)    rx_packets    rx_dropped    tx_packets    "
      "tx_dropped\n"
      "------  --------------  ------------  ------------  ------------  "
      "------------\n");
  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (!kni_port_params_array[i]) continue;

    printf("%7d %10u/%2u %13" PRIu64 " %13" PRIu64 " %13" PRIu64
           " "
           "%13" PRIu64 "\n",
           i, kni_port_params_array[i]->lcore_rx,
           kni_port_params_array[i]->lcore_tx, kni_stats[i].rx_packets,
           kni_stats[i].rx_dropped, kni_stats[i].tx_packets,
           kni_stats[i].tx_dropped);
  }
  printf(
      "======  ==============  ============  ============  ============  "
      "============\n");
}

/* Custom handling of signals to handle stats and kni processing */
static void signal_handler(int signum) {
  /* When we receive a USR1 signal, print stats */
  if (signum == SIGUSR1) {
    print_stats();
  }

  /* When we receive a USR2 signal, reset stats */
  if (signum == SIGUSR2) {
    memset(&kni_stats, 0, sizeof(kni_stats));
    printf("\n** Statistics have been reset **\n");
    return;
  }

  /*
   * When we receive a RTMIN or SIGINT or SIGTERM signal,
   * stop kni processing
   */
  if (signum == SIGRTMIN || signum == SIGINT || signum == SIGTERM) {
    printf(
        "\nSIGRTMIN/SIGINT/SIGTERM received. "
        "KNI processing stopping.\n");
    rte_atomic32_inc(&kni_stop);
    return;
  }
}

static void kni_burst_free_mbufs(struct rte_mbuf **pkts, unsigned num) {
  unsigned i;

  if (pkts == NULL) return;

  for (i = 0; i < num; i++) {
    rte_pktmbuf_free(pkts[i]);
    pkts[i] = NULL;
  }
}

/**
 * Interface to burst rx and enqueue mbufs into rx_q
 */
static void kni_ingress(struct kni_port_params *p) {
  uint8_t i;
  uint16_t port_id;
  unsigned nb_rx, num;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

  if (p == NULL) return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (i = 0; i < nb_kni; i++) {
    /* Burst rx from eth */
    nb_rx = rte_eth_rx_burst(port_id, 0, pkts_burst, PKT_BURST_SZ);
    if (unlikely(nb_rx > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from eth\n");
      return;
    }
    kni_stats[port_id].rx_packets += nb_rx;

    /* Burst rx to worker ring */
    num = rte_ring_enqueue_burst(worker_rx_ring, (void **)pkts_burst, nb_rx,
                                 NULL);
    if (unlikely(num < nb_rx)) {
      // Free mbufs not tx to NIC
      kni_burst_free_mbufs(&pkts_burst[num], nb_rx - num);
      kni_stats[port_id].rx_dropped += nb_rx - num;
    }
  }
}

/**
 * Interface to dequeue mbufs from tx_q and burst tx
 */
static void kni_egress(struct kni_port_params *p) {
  uint8_t i;
  uint16_t port_id;
  unsigned int nb_tx, nb_rx;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;

  if (p == NULL) return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;

  int good_nums;
  for (i = 0; i < nb_kni; i++) {
    // Burst rx from kni
    nb_rx = rte_kni_rx_burst(p->kni[i], pkts_burst, PKT_BURST_SZ);
    if (unlikely(nb_rx > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from KNI\n");
      return;
    }

    // Burst tx to eth
    nb_tx = rte_eth_tx_burst(port_id, 0, pkts_burst, (uint16_t)nb_rx);
    if (nb_tx) kni_stats[port_id].tx_packets += nb_tx;
    if (unlikely(nb_tx < nb_rx)) {
      // Free mbufs not tx to NIC
      kni_burst_free_mbufs(&pkts_burst[nb_tx], nb_rx - nb_tx);
      kni_stats[port_id].tx_dropped += nb_rx - nb_tx;
    }
  }
}

// Read from kni_ingress and send non-DNS packets to worker_egress
static void worker_ingress(struct kni_port_params *p) {
  struct rte_mbuf *buf[16] __rte_cache_aligned;
  int32_t f_stop, f_pause;
  uint16_t port_id;
  uint32_t nb_kni;

  if (p == NULL) return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (int i = 0; i < nb_kni; i++) {
    // -1 is not set, 1 is NXDOMAIN, 0 is normal
    int8_t packet_status[PKT_BURST_SZ] = {-1};
    // Burst RX from ring
    unsigned int nb_rx = rte_ring_dequeue_burst(worker_rx_ring, (void **)buf,
                                                PKT_BURST_SZ, nullptr);
    if (unlikely(nb_rx > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from worker\n");
      return;
    }

    // Flag packets we can answer
    unsigned int bad_packets = 0;
    for (uint32_t it = 0; it < nb_rx; it++) {
      if (check_if_query(buf[it])) {
        if (!check_if_tld_valid(buf[it])) {
          packet_status[it] = 1;
          bad_packets++;
        } else {
          packet_status[it] = 0;
        }
      } else {
        packet_status[it] = 0;
      }
    }

    // Pass bad packets to worker_egress
    struct rte_mbuf *bad_pkt_buf[PKT_BURST_SZ] __rte_cache_aligned;
    unsigned int counter = 0;
    for (int it = 0; it < nb_rx; it++) {
      if (packet_status[it] == 1) {
        bad_pkt_buf[counter] = buf[it];
        counter++;
      }
    }

    unsigned int num_bad_tx = rte_ring_enqueue_burst(
        worker_tx_ring, (void **)bad_pkt_buf, bad_packets, nullptr);
    if (unlikely(num_bad_tx < bad_packets)) {
      // Free mbufs not tx to ring interface
      kni_burst_free_mbufs(&bad_pkt_buf[num_bad_tx], bad_packets - num_bad_tx);
    }

    // Pass good packets to KNI
    struct rte_mbuf *good_pkt_buf[PKT_BURST_SZ] __rte_cache_aligned;
    counter = 0;

    for (int it = 0; it < nb_rx; it++) {
      if (packet_status[it] == 0) {
        good_pkt_buf[counter] = buf[it];
        counter++;
      }
    }

    int good_packets = nb_rx - bad_packets;
    unsigned int num_good_tx =
        rte_kni_tx_burst(p->kni[i], good_pkt_buf, good_packets);

    rte_kni_handle_request(p->kni[i]);
    if (unlikely(num_good_tx < good_packets)) {
      // Free mbufs not tx to kni interface
      kni_burst_free_mbufs(&good_pkt_buf[num_good_tx],
                           good_packets - num_good_tx);
      kni_stats[port_id].rx_dropped += good_packets - num_good_tx;
    }
  }
}

// Write invalid TLD responses to rte_eth_tx
static void worker_egress(struct kni_port_params *p) {
  uint8_t i;
  uint16_t port_id;
  unsigned int nb_tx, num, nb_rx;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;

  if (p == NULL) return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;

  int good_nums;
  for (i = 0; i < nb_kni; i++) {
    // Burst rx from kni
    num = rte_ring_dequeue_burst(worker_tx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
    if (unlikely(num > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from KNI\n");
      return;
    }

    // Create replies
    for (int it = 0; it < num; it++) {
      create_nxdomain_reply(pkts_burst[i]);
    }

    // Burst tx to eth with replies
    nb_tx = rte_eth_tx_burst(port_id, 0, pkts_burst, (uint16_t)num);
    if (nb_tx) kni_stats[port_id].tx_packets += nb_tx;
    if (unlikely(nb_tx < num)) {
      // Free mbufs not tx to NIC
      kni_burst_free_mbufs(&pkts_burst[nb_tx], num - nb_tx);
      kni_stats[port_id].tx_dropped += num - nb_tx;
    }
  }
}

static int main_loop(__rte_unused void *arg) {
  uint16_t i;
  int32_t f_stop;
  int32_t f_pause;
  const unsigned lcore_id = rte_lcore_id();
  enum lcore_rxtx {
    LCORE_NONE,
    LCORE_RX,
    LCORE_TX,
    WORKER_TRANSLATE,
    WORKER_VTOP,
    LCORE_MAX
  };
  enum lcore_rxtx flag = LCORE_NONE;

  RTE_ETH_FOREACH_DEV(i) {
    if (!kni_port_params_array[i]) continue;
    if (kni_port_params_array[i]->lcore_rx == (uint8_t)lcore_id) {
      flag = LCORE_RX;
      break;
    } else if (kni_port_params_array[i]->lcore_tx == (uint8_t)lcore_id) {
      flag = LCORE_TX;
      break;
    } else if (kni_port_params_array[i]->lcore_worker_translate ==
               (uint8_t)lcore_id) {
      flag = WORKER_TRANSLATE;
      break;
    } else if (kni_port_params_array[i]->lcore_worker_vtop ==
               (uint8_t)lcore_id) {
      flag = WORKER_VTOP;
      break;
    }
  }

  if (flag == LCORE_RX) {
    RTE_LOG(INFO, APP, "Lcore %u is reading from port %d\n",
            kni_port_params_array[i]->lcore_rx,
            kni_port_params_array[i]->port_id);
    while (1) {
      f_stop = rte_atomic32_read(&kni_stop);
      f_pause = rte_atomic32_read(&kni_pause);
      if (f_stop) break;
      if (f_pause) continue;
      kni_ingress(kni_port_params_array[i]);
    }
  } else if (flag == LCORE_TX) {
    RTE_LOG(INFO, APP, "Lcore %u is writing to port %d\n",
            kni_port_params_array[i]->lcore_tx,
            kni_port_params_array[i]->port_id);
    while (1) {
      f_stop = rte_atomic32_read(&kni_stop);
      f_pause = rte_atomic32_read(&kni_pause);
      if (f_stop) break;
      if (f_pause) continue;
      kni_egress(kni_port_params_array[i]);
    }
  } else if (flag == WORKER_TRANSLATE) {
    RTE_LOG(INFO, APP, "Lcore %u is WORKER_TRANSLATE\n",
            kni_port_params_array[i]->lcore_tx + 1);
    while (1) {
      f_stop = rte_atomic32_read(&kni_stop);
      f_pause = rte_atomic32_read(&kni_pause);
      if (f_stop) break;
      if (f_pause) continue;
      worker_egress(kni_port_params_array[i]);
    }
  } else if (flag == WORKER_VTOP) {
    RTE_LOG(INFO, APP, "Lcore %u is WORKER_VTOP\n",
            kni_port_params_array[i]->lcore_tx + 2);
    while (1) {
      f_stop = rte_atomic32_read(&kni_stop);
      f_pause = rte_atomic32_read(&kni_pause);
      if (f_stop) break;
      if (f_pause) continue;
      verilator_top_worker(worker_tx_ring, worker_rx_ring);
    }
  } else
    RTE_LOG(INFO, APP, "Lcore %u has nothing to do\n", lcore_id);

  return 0;
}

// Display usage instructions
static void print_usage(const char *prgname) {
  RTE_LOG(
      INFO, APP,
      "\nUsage: %s [EAL options] -- -p PORTMASK -P -m "
      "[--config "
      "(port,lcore_rx,lcore_tx,lcore_worker_translate,lcore_worker_vtop,"
      "lcore_kthread...)"
      "[,(port,lcore_rx,lcore_tx,lcore_kthread...)]]\n"
      "    -p PORTMASK: hex bitmask of ports to use\n"
      "    -P : enable promiscuous mode\n"
      "    -m : enable monitoring of port carrier state\n"
      "    --config "
      "(port,lcore_rx,lcore_tx,lcore_worker_translate,lcore_worker_vtop,lcore_"
      "kthread...): "
      "port and lcore configurations\n",
      prgname);
}

// Convert string to unsigned number. 0 is returned if error occurs
static uint32_t parse_unsigned(const char *portmask) {
  char *end = NULL;
  unsigned long num;

  num = strtoul(portmask, &end, 16);

  if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0')) return 0;

  return (uint32_t)num;
}

static void print_config(void) {
  uint32_t i, j;
  struct kni_port_params **p = kni_port_params_array;

  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (!p[i]) continue;
    RTE_LOG(DEBUG, APP, "Port ID: %d\n", p[i]->port_id);
    RTE_LOG(DEBUG, APP, "Rx lcore ID: %u, Tx lcore ID: %u\n", p[i]->lcore_rx,
            p[i]->lcore_tx);
    for (j = 0; j < p[i]->nb_lcore_k; j++)
      RTE_LOG(DEBUG, APP, "Kernel thread lcore ID: %u\n", p[i]->lcore_k[j]);
  }
}

static int parse_config(const char *arg) {
  const char *p, *p0 = arg;
  char s[256], *end;
  unsigned size;
  enum fieldnames {
    FLD_PORT = 0,
    FLD_LCORE_RX,
    FLD_LCORE_TX,
    FLD_LCORE_WORKER_TRANSLATE,
    FLD_LCORE_WORKER_VTOP,
    _NUM_FLD = KNI_MAX_KTHREAD + 3,
  };
  int i, j, nb_token;
  char *str_fld[_NUM_FLD];
  unsigned long int_fld[_NUM_FLD];
  uint16_t port_id, nb_kni_port_params = 0;

  memset(&kni_port_params_array, 0, sizeof(kni_port_params_array));
  while (((p = strchr(p0, '(')) != NULL) &&
         nb_kni_port_params < RTE_MAX_ETHPORTS) {
    p++;
    if ((p0 = strchr(p, ')')) == NULL) goto fail;
    size = p0 - p;
    if (size >= sizeof(s)) {
      printf("Invalid config parameters\n");
      goto fail;
    }
    snprintf(s, sizeof(s), "%.*s", size, p);
    nb_token = rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',');
    if (nb_token <= FLD_LCORE_WORKER_VTOP) {
      printf("Invalid config parameters\n");
      goto fail;
    }
    for (i = 0; i < nb_token; i++) {
      errno = 0;
      int_fld[i] = strtoul(str_fld[i], &end, 0);
      if (errno != 0 || end == str_fld[i]) {
        printf("Invalid config parameters\n");
        goto fail;
      }
    }

    i = 0;
    port_id = int_fld[i++];
    if (port_id >= RTE_MAX_ETHPORTS) {
      printf("Port ID %d could not exceed the maximum %d\n", port_id,
             RTE_MAX_ETHPORTS);
      goto fail;
    }
    if (kni_port_params_array[port_id]) {
      printf("Port %d has been configured\n", port_id);
      goto fail;
    }
    kni_port_params_array[port_id] = (kni_port_params *)rte_zmalloc(
        "KNI_port_params", sizeof(struct kni_port_params), RTE_CACHE_LINE_SIZE);
    kni_port_params_array[port_id]->port_id = port_id;
    kni_port_params_array[port_id]->lcore_rx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_tx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_worker_vtop = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_worker_translate =
        (uint8_t)int_fld[i++];
    if (kni_port_params_array[port_id]->lcore_rx >= RTE_MAX_LCORE ||
        kni_port_params_array[port_id]->lcore_tx >= RTE_MAX_LCORE ||
        kni_port_params_array[port_id]->lcore_worker_vtop >= RTE_MAX_LCORE ||
        kni_port_params_array[port_id]->lcore_worker_translate >=
            RTE_MAX_LCORE) {
      printf(
          "lcore_rx %u or lcore_tx %u ID  or lcore_worker_vtop %u or "
          "lcore_worker_translate %u could not exceed the maximum %u\n",
          kni_port_params_array[port_id]->lcore_rx,
          kni_port_params_array[port_id]->lcore_tx,
          kni_port_params_array[port_id]->lcore_worker_vtop,
          kni_port_params_array[port_id]->lcore_worker_translate,
          (unsigned)RTE_MAX_LCORE);
      goto fail;
    }
    for (j = 0; i < nb_token && j < KNI_MAX_KTHREAD; i++, j++)
      kni_port_params_array[port_id]->lcore_k[j] = (uint8_t)int_fld[i];
    kni_port_params_array[port_id]->nb_lcore_k = j;
  }
  print_config();

  return 0;

fail:
  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (kni_port_params_array[i]) {
      rte_free(kni_port_params_array[i]);
      kni_port_params_array[i] = NULL;
    }
  }

  return -1;
}

static int validate_parameters(uint32_t portmask) {
  uint32_t i;

  if (!portmask) {
    printf("No port configured in port mask\n");
    return -1;
  }

  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (((portmask & (1 << i)) && !kni_port_params_array[i]) ||
        (!(portmask & (1 << i)) && kni_port_params_array[i]))
      rte_exit(EXIT_FAILURE,
               "portmask is not consistent "
               "to port ids specified in --config\n");

    if (kni_port_params_array[i] &&
        !rte_lcore_is_enabled((unsigned)(kni_port_params_array[i]->lcore_rx)))
      rte_exit(EXIT_FAILURE,
               "lcore id %u for "
               "port %d receiving not enabled\n",
               kni_port_params_array[i]->lcore_rx,
               kni_port_params_array[i]->port_id);

    if (kni_port_params_array[i] &&
        !rte_lcore_is_enabled((unsigned)(kni_port_params_array[i]->lcore_tx)))
      rte_exit(EXIT_FAILURE,
               "lcore id %u for "
               "port %d transmitting not enabled\n",
               kni_port_params_array[i]->lcore_tx,
               kni_port_params_array[i]->port_id);

    if (kni_port_params_array[i] &&
        !rte_lcore_is_enabled(
            (unsigned)(kni_port_params_array[i]->lcore_worker_vtop)))
      rte_exit(EXIT_FAILURE,
               "lcore id %u for "
               "worker rx not enabled\n",
               kni_port_params_array[i]->lcore_worker_vtop);

    if (kni_port_params_array[i] &&
        !rte_lcore_is_enabled(
            (unsigned)(kni_port_params_array[i]->lcore_worker_translate)))
      rte_exit(EXIT_FAILURE,
               "lcore id %u for "
               "worker tx not enabled\n",
               kni_port_params_array[i]->lcore_worker_translate);
  }

  return 0;
}

#define CMDLINE_OPT_CONFIG "config"

// Parse the arguments given in the command line of the application
static int parse_args(int argc, char **argv) {
  int opt, longindex, ret = 0;
  const char *prgname = argv[0];
  static struct option longopts[] = {
      {CMDLINE_OPT_CONFIG, required_argument, NULL, 0}, {NULL, 0, NULL, 0}};

  // Disable printing messages within getopt()
  opterr = 0;

  // Parse command line
  while ((opt = getopt_long(argc, argv, "p:Pm", longopts, &longindex)) != EOF) {
    switch (opt) {
      case 'p':
        ports_mask = parse_unsigned(optarg);
        break;
      case 'P':
        promiscuous_on = 1;
        break;
      case 'm':
        monitor_links = 1;
        break;
      case 0:
        if (!strncmp(longopts[longindex].name, CMDLINE_OPT_CONFIG,
                     sizeof(CMDLINE_OPT_CONFIG))) {
          ret = parse_config(optarg);
          if (ret) {
            printf("Invalid config\n");
            print_usage(prgname);
            return -1;
          }
        }
        break;
      default:
        print_usage(prgname);
        rte_exit(EXIT_FAILURE, "Invalid option specified\n");
    }
  }

  // Check that options were parsed ok
  if (validate_parameters(ports_mask) < 0) {
    print_usage(prgname);
    rte_exit(EXIT_FAILURE, "Invalid parameters\n");
  }

  return ret;
}

// Initialize KNI subsystem
static void init_kni(void) {
  unsigned int num_of_kni_ports = 0, i;
  struct kni_port_params **params = kni_port_params_array;

  // Calculate the maximum number of KNI interfaces that will be used
  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (kni_port_params_array[i]) {
      num_of_kni_ports += (params[i]->nb_lcore_k ? params[i]->nb_lcore_k : 1);
    }
  }

  // Invoke rte KNI init to preallocate the ports
  rte_kni_init(num_of_kni_ports);
}

// Initialise a single port on an Ethernet device
static void init_port(uint16_t port) {
  int ret;
  uint16_t nb_rxd = NB_RXD;
  uint16_t nb_txd = NB_TXD;
  struct rte_eth_dev_info dev_info;
  struct rte_eth_rxconf rxq_conf;
  struct rte_eth_txconf txq_conf;
  struct rte_eth_conf local_port_conf = port_conf;

  // Initialise device and RX/TX queues
  RTE_LOG(INFO, APP, "Initialising port %u ...\n", (unsigned)port);
  fflush(stdout);

  ret = rte_eth_dev_info_get(port, &dev_info);
  if (ret != 0)
    rte_exit(EXIT_FAILURE, "Error during getting device (port %u) info: %s\n",
             port, strerror(-ret));

  if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
    local_port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
  ret = rte_eth_dev_configure(port, 1, 1, &local_port_conf);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Could not configure port%u (%d)\n", (unsigned)port,
             ret);

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
  if (ret < 0)
    rte_exit(EXIT_FAILURE,
             "Could not adjust number of descriptors "
             "for port%u (%d)\n",
             (unsigned)port, ret);

  rxq_conf = dev_info.default_rxconf;
  rxq_conf.offloads = local_port_conf.rxmode.offloads;
  ret = rte_eth_rx_queue_setup(port, 0, nb_rxd, rte_eth_dev_socket_id(port),
                               &rxq_conf, pktmbuf_pool);
  if (ret < 0)
    rte_exit(EXIT_FAILURE,
             "Could not setup up RX queue for "
             "port%u (%d)\n",
             (unsigned)port, ret);

  txq_conf = dev_info.default_txconf;
  txq_conf.offloads = local_port_conf.txmode.offloads;
  ret = rte_eth_tx_queue_setup(port, 0, nb_txd, rte_eth_dev_socket_id(port),
                               &txq_conf);
  if (ret < 0)
    rte_exit(EXIT_FAILURE,
             "Could not setup up TX queue for "
             "port%u (%d)\n",
             (unsigned)port, ret);

  ret = rte_eth_dev_start(port);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Could not start port%u (%d)\n", (unsigned)port,
             ret);

  if (promiscuous_on) {
    ret = rte_eth_promiscuous_enable(port);
    if (ret != 0)
      rte_exit(EXIT_FAILURE,
               "Could not enable promiscuous mode for port%u: %s\n", port,
               rte_strerror(-ret));
  }
}

// Check the link status of all ports in up to 9s, and print them finally
static void check_all_ports_link_status(uint32_t port_mask) {
#define CHECK_INTERVAL 100  // 100ms
#define MAX_CHECK_TIME 90   // 9s (90 * 100ms) in total
  uint16_t portid;
  uint8_t count, all_ports_up, print_flag = 0;
  struct rte_eth_link link;
  int ret;

  printf("\nChecking link status\n");
  fflush(stdout);
  for (count = 0; count <= MAX_CHECK_TIME; count++) {
    all_ports_up = 1;
    RTE_ETH_FOREACH_DEV(portid) {
      if ((port_mask & (1 << portid)) == 0) continue;
      memset(&link, 0, sizeof(link));
      ret = rte_eth_link_get_nowait(portid, &link);
      if (ret < 0) {
        all_ports_up = 0;
        if (print_flag == 1)
          printf("Port %u link get failed: %s\n", portid, rte_strerror(-ret));
        continue;
      }
      // print link status if flag set
      if (print_flag == 1) {
        if (link.link_status)
          printf(
              "Port%d Link Up - speed %uMbps - %s\n", portid, link.link_speed,
              (link.link_duplex == ETH_LINK_FULL_DUPLEX) ? ("full-duplex")
                                                         : ("half-duplex\n"));
        else
          printf("Port %d Link Down\n", portid);
        continue;
      }
      // clear all_ports_up flag if any link down
      if (link.link_status == ETH_LINK_DOWN) {
        all_ports_up = 0;
        break;
      }
    }
    // after finally printing all link status, get out
    if (print_flag == 1) break;

    if (all_ports_up == 0) {
      printf(".");
      fflush(stdout);
      rte_delay_ms(CHECK_INTERVAL);
    }

    // set the print_flag if all ports up or timeout
    if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
      print_flag = 1;
      printf("done\n");
    }
  }
}

static void log_link_state(struct rte_kni *kni, int prev,
                           struct rte_eth_link *link) {
  if (kni == NULL || link == NULL) return;

  if (prev == ETH_LINK_DOWN && link->link_status == ETH_LINK_UP) {
    RTE_LOG(INFO, APP, "%s NIC Link is Up %d Mbps %s %s.\n",
            rte_kni_get_name(kni), link->link_speed,
            link->link_autoneg ? "(AutoNeg)" : "(Fixed)",
            link->link_duplex ? "Full Duplex" : "Half Duplex");
  } else if (prev == ETH_LINK_UP && link->link_status == ETH_LINK_DOWN) {
    RTE_LOG(INFO, APP, "%s NIC Link is Down.\n", rte_kni_get_name(kni));
  }
}

/*
 * Monitor the link status of all ports and update the
 * corresponding KNI interface(s)
 */
static void *monitor_all_ports_link_status(void *arg) {
  uint16_t portid;
  struct rte_eth_link link;
  unsigned int i;
  struct kni_port_params **p = kni_port_params_array;
  int prev;
  (void)arg;
  int ret;

  while (monitor_links) {
    rte_delay_ms(500);
    RTE_ETH_FOREACH_DEV(portid) {
      if ((ports_mask & (1 << portid)) == 0) continue;
      memset(&link, 0, sizeof(link));
      ret = rte_eth_link_get_nowait(portid, &link);
      if (ret < 0) {
        RTE_LOG(ERR, APP, "Get link failed (port %u): %s\n", portid,
                rte_strerror(-ret));
        continue;
      }
      for (i = 0; i < p[portid]->nb_kni; i++) {
        prev = rte_kni_update_link(p[portid]->kni[i], link.link_status);
        log_link_state(p[portid]->kni[i], prev, &link);
      }
    }
  }
  return NULL;
}

static int kni_change_mtu_(uint16_t port_id, unsigned int new_mtu) {
  int ret;
  uint16_t nb_rxd = NB_RXD;
  uint16_t nb_txd = NB_TXD;
  struct rte_eth_conf conf;
  struct rte_eth_dev_info dev_info;
  struct rte_eth_rxconf rxq_conf;
  struct rte_eth_txconf txq_conf;

  if (!rte_eth_dev_is_valid_port(port_id)) {
    RTE_LOG(ERR, APP, "Invalid port id %d\n", port_id);
    return -EINVAL;
  }

  RTE_LOG(INFO, APP, "Change MTU of port %d to %u\n", port_id, new_mtu);

  /* Stop specific port */
  ret = rte_eth_dev_stop(port_id);
  if (ret != 0) {
    RTE_LOG(ERR, APP, "Failed to stop port %d: %s\n", port_id,
            rte_strerror(-ret));
    return ret;
  }

  memcpy(&conf, &port_conf, sizeof(conf));

  conf.rxmode.mtu = new_mtu;
  ret = rte_eth_dev_configure(port_id, 1, 1, &conf);
  if (ret < 0) {
    RTE_LOG(ERR, APP, "Fail to reconfigure port %d\n", port_id);
    return ret;
  }

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
  if (ret < 0)
    rte_exit(EXIT_FAILURE,
             "Could not adjust number of descriptors "
             "for port%u (%d)\n",
             (unsigned int)port_id, ret);

  ret = rte_eth_dev_info_get(port_id, &dev_info);
  if (ret != 0) {
    RTE_LOG(ERR, APP, "Error during getting device (port %u) info: %s\n",
            port_id, strerror(-ret));

    return ret;
  }

  rxq_conf = dev_info.default_rxconf;
  rxq_conf.offloads = conf.rxmode.offloads;
  ret =
      rte_eth_rx_queue_setup(port_id, 0, nb_rxd, rte_eth_dev_socket_id(port_id),
                             &rxq_conf, pktmbuf_pool);
  if (ret < 0) {
    RTE_LOG(ERR, APP, "Fail to setup Rx queue of port %d\n", port_id);
    return ret;
  }

  txq_conf = dev_info.default_txconf;
  txq_conf.offloads = conf.txmode.offloads;
  ret = rte_eth_tx_queue_setup(port_id, 0, nb_txd,
                               rte_eth_dev_socket_id(port_id), &txq_conf);
  if (ret < 0) {
    RTE_LOG(ERR, APP, "Fail to setup Tx queue of port %d\n", port_id);
    return ret;
  }

  /* Restart specific port */
  ret = rte_eth_dev_start(port_id);
  if (ret < 0) {
    RTE_LOG(ERR, APP, "Fail to restart port %d\n", port_id);
    return ret;
  }

  return 0;
}

/* Callback for request of configuring network interface up/down */
static int kni_config_network_interface(uint16_t port_id, uint8_t if_up) {
  int ret = 0;

  if (!rte_eth_dev_is_valid_port(port_id)) {
    RTE_LOG(ERR, APP, "Invalid port id %d\n", port_id);
    return -EINVAL;
  }

  RTE_LOG(INFO, APP, "Configure network interface of %d %s\n", port_id,
          if_up ? "up" : "down");

  rte_atomic32_inc(&kni_pause);

  if (if_up != 0) {
    /* Configure network interface up */
    rte_eth_dev_stop(port_id);
    ret = rte_eth_dev_start(port_id);
  } else /* Configure network interface down */
    rte_eth_dev_stop(port_id);

  rte_atomic32_dec(&kni_pause);

  if (ret < 0) RTE_LOG(ERR, APP, "Failed to start port %d\n", port_id);

  return ret;
}

static void print_ethaddr(const char *name, struct rte_ether_addr *mac_addr) {
  char buf[RTE_ETHER_ADDR_FMT_SIZE];
  rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, mac_addr);
  RTE_LOG(INFO, APP, "\t%s%s\n", name, buf);
}

/* Callback for request of configuring mac address */
static int kni_config_mac_address(uint16_t port_id, uint8_t mac_addr[]) {
  int ret = 0;

  if (!rte_eth_dev_is_valid_port(port_id)) {
    RTE_LOG(ERR, APP, "Invalid port id %d\n", port_id);
    return -EINVAL;
  }

  RTE_LOG(INFO, APP, "Configure mac address of %d\n", port_id);
  print_ethaddr("Address:", (struct rte_ether_addr *)mac_addr);

  ret = rte_eth_dev_default_mac_addr_set(port_id,
                                         (struct rte_ether_addr *)mac_addr);
  if (ret < 0)
    RTE_LOG(ERR, APP, "Failed to config mac_addr for port %d\n", port_id);

  return ret;
}

static int kni_alloc(uint16_t port_id) {
  uint8_t i;
  struct rte_kni *kni;
  struct rte_kni_conf conf;
  struct kni_port_params **params = kni_port_params_array;
  int ret;

  if (port_id >= RTE_MAX_ETHPORTS || !params[port_id]) return -1;

  params[port_id]->nb_kni =
      params[port_id]->nb_lcore_k ? params[port_id]->nb_lcore_k : 1;

  for (i = 0; i < params[port_id]->nb_kni; i++) {
    /* Clear conf at first */
    memset(&conf, 0, sizeof(conf));
    if (params[port_id]->nb_lcore_k) {
      snprintf(conf.name, RTE_KNI_NAMESIZE, "vEth%u_%u", port_id, i);
      conf.core_id = params[port_id]->lcore_k[i];
      conf.force_bind = 1;
    } else
      snprintf(conf.name, RTE_KNI_NAMESIZE, "vEth%u", port_id);
    conf.group_id = port_id;
    conf.mbuf_size = MAX_PACKET_SZ;
    /*
     * The first KNI device associated to a port
     * is the master, for multiple kernel thread
     * environment.
     */
    if (i == 0) {
      struct rte_kni_ops ops;
      struct rte_eth_dev_info dev_info;

      ret = rte_eth_dev_info_get(port_id, &dev_info);
      if (ret != 0)
        rte_exit(EXIT_FAILURE,
                 "Error during getting device (port %u) info: %s\n", port_id,
                 strerror(-ret));

      /* Get the interface default mac address */
      ret =
          rte_eth_macaddr_get(port_id, (struct rte_ether_addr *)&conf.mac_addr);
      if (ret != 0)
        rte_exit(EXIT_FAILURE, "Failed to get MAC address (port %u): %s\n",
                 port_id, rte_strerror(-ret));

      rte_eth_dev_get_mtu(port_id, &conf.mtu);

      conf.min_mtu = dev_info.min_mtu;
      conf.max_mtu = dev_info.max_mtu;

      memset(&ops, 0, sizeof(ops));
      ops.port_id = port_id;
      ops.change_mtu = kni_change_mtu;
      ops.config_network_if = kni_config_network_interface;
      ops.config_mac_address = kni_config_mac_address;

      kni = rte_kni_alloc(pktmbuf_pool, &conf, &ops);
    } else
      kni = rte_kni_alloc(pktmbuf_pool, &conf, NULL);

    if (!kni)
      rte_exit(EXIT_FAILURE,
               "Fail to create kni for "
               "port: %d\n",
               port_id);
    params[port_id]->kni[i] = kni;
  }

  return 0;
}

static int kni_free_kni(uint16_t port_id) {
  uint8_t i;
  struct kni_port_params **p = kni_port_params_array;

  if (port_id >= RTE_MAX_ETHPORTS || !p[port_id]) return -1;

  for (i = 0; i < p[port_id]->nb_kni; i++) {
    if (rte_kni_release(p[port_id]->kni[i])) printf("Fail to release kni\n");
    p[port_id]->kni[i] = NULL;
  }
  rte_eth_dev_stop(port_id);

  return 0;
}

/* Initialise ports/queues etc. and start main loop on each core */
int main(int argc, char **argv) {
  int ret;
  uint16_t nb_sys_ports, port;
  unsigned i;
  void *retval;
  pthread_t kni_link_tid;
  int pid;

  tx_mode.mq_mode = ETH_MQ_TX_NONE;
  port_conf.txmode = tx_mode;

  /* Associate signal_hanlder function with USR signals */
  signal(SIGUSR1, signal_handler);
  signal(SIGUSR2, signal_handler);
  signal(SIGRTMIN, signal_handler);
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* Initialise EAL */
  ret = rte_eal_init(argc, argv);
  if (ret < 0) rte_exit(EXIT_FAILURE, "Could not initialise EAL (%d)\n", ret);
  argc -= ret;
  argv += ret;

  /* Parse application arguments (after the EAL ones) */
  ret = parse_args(argc, argv);
  if (ret < 0) rte_exit(EXIT_FAILURE, "Could not parse input parameters\n");

  /* Create the mbuf pool */
  pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF, MEMPOOL_CACHE_SZ,
                                         0, MBUF_DATA_SZ, rte_socket_id());
  if (pktmbuf_pool == NULL) {
    rte_exit(EXIT_FAILURE, "Could not initialise mbuf pool\n");
    return -1;
  }

  // Create ring buffers
  worker_tx_ring = rte_ring_create("Worker ring output", 1024, rte_socket_id(),
                                   RING_F_SC_DEQ);
  worker_rx_ring = rte_ring_create("Worker ring input", 1024, rte_socket_id(),
                                   RING_F_SC_DEQ);

  /* Get number of ports found in scan */
  nb_sys_ports = rte_eth_dev_count_avail();
  if (nb_sys_ports == 0)
    rte_exit(EXIT_FAILURE, "No supported Ethernet device found\n");

  /* Check if the configured port ID is valid */
  for (i = 0; i < RTE_MAX_ETHPORTS; i++)
    if (kni_port_params_array[i] && !rte_eth_dev_is_valid_port(i))
      rte_exit(EXIT_FAILURE,
               "Configured invalid "
               "port ID %u\n",
               i);

  /* Initialize KNI subsystem */
  init_kni();

  /* Initialize Verilator module */
  init_verilated_top();

  /* Initialise each port */
  RTE_ETH_FOREACH_DEV(port) {
    /* Skip ports that are not enabled */
    if (!(ports_mask & (1 << port))) continue;
    init_port(port);

    if (port >= RTE_MAX_ETHPORTS)
      rte_exit(EXIT_FAILURE,
               "Can not use more than "
               "%d ports for kni\n",
               RTE_MAX_ETHPORTS);

    kni_alloc(port);
  }
  check_all_ports_link_status(ports_mask);

  pid = getpid();
  RTE_LOG(INFO, APP, "========================\n");
  RTE_LOG(INFO, APP, "KNI Running\n");
  RTE_LOG(INFO, APP, "kill -SIGUSR1 %d\n", pid);
  RTE_LOG(INFO, APP, "    Show KNI Statistics.\n");
  RTE_LOG(INFO, APP, "kill -SIGUSR2 %d\n", pid);
  RTE_LOG(INFO, APP, "    Zero KNI Statistics.\n");
  RTE_LOG(INFO, APP, "========================\n");
  fflush(stdout);

  ret = rte_ctrl_thread_create(&kni_link_tid, "KNI link status check", NULL,
                               monitor_all_ports_link_status, NULL);
  if (ret < 0) rte_exit(EXIT_FAILURE, "Could not create link status thread!\n");

  /* Launch per-lcore function on every lcore */
  rte_eal_mp_remote_launch(main_loop, NULL, CALL_MAIN);
  RTE_LCORE_FOREACH_WORKER(i) {
    if (rte_eal_wait_lcore(i) < 0) return -1;
  }
  monitor_links = 0;
  pthread_join(kni_link_tid, &retval);

  /* Release resources */
  RTE_ETH_FOREACH_DEV(port) {
    if (!(ports_mask & (1 << port))) continue;
    kni_free_kni(port);
  }
  stop_verilated_top();
  for (i = 0; i < RTE_MAX_ETHPORTS; i++)
    if (kni_port_params_array[i]) {
      rte_free(kni_port_params_array[i]);
      kni_port_params_array[i] = NULL;
    }

  return 0;
}
