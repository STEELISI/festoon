/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <rte_branch_prediction.h>
#include <rte_bus_pci.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_interrupts.h>
#include <rte_kni.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_per_lcore.h>
#include <rte_string_fns.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <unistd.h>

#include "festoon_eth.h"
#include "festoon_kni.h"
#include "festoon_top.h"
#include "festoon_xgmii.h"
#include "params.h"

kni_port_params *kni_port_params_array[RTE_MAX_ETHPORTS];

/* Options for configuring ethernet port */
struct rte_eth_conf port_conf = {
    .txmode =
        {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
        },
};

/* Mempool for mbufs */
rte_mempool *pktmbuf_pool = NULL, *xgmii_pool = NULL;

/* Mask of enabled ports */
uint32_t ports_mask = 0;
/* Ports set in promiscuous mode off by default. */
int promiscuous_on = 0;
/* Monitor link status continually. off by default. */
int monitor_links;

/* kni device statistics array */
kni_interface_stats kni_stats[RTE_MAX_ETHPORTS];

int kni_change_mtu(uint16_t port_id, unsigned int new_mtu);
int kni_config_network_interface(uint16_t port_id, uint8_t if_up);
int kni_config_mac_address(uint16_t port_id, uint8_t mac_addr[]);

uint32_t kni_stop, kni_pause;

bool eth_pkt_start, pci_pkt_start;

rte_ring *eth_tx_ring, *eth_rx_ring, *kni_tx_ring, *kni_rx_ring;

/* Print out statistics on packets handled */
void print_stats(void) {
  uint16_t i;

  printf("\n**KNI example application statistics**\n"
         "======  ==============  ============  ============  ============  "
         "============\n"
         " Port    Lcore(RX/TX)    rx_packets    rx_dropped    tx_packets    "
         "tx_dropped\n"
         "------  --------------  ------------  ------------  ------------  "
         "------------\n");
  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (!kni_port_params_array[i])
      continue;

    printf("%7d %10u/%2u %13" PRIu64 " %13" PRIu64 " %13" PRIu64 " "
           "%13" PRIu64 "\n ",
           i, kni_port_params_array[i]->lcore_eth_rx,
           kni_port_params_array[i]->lcore_eth_tx, kni_stats[i].rx_packets,
           kni_stats[i].rx_dropped, kni_stats[i].tx_packets,
           kni_stats[i].tx_dropped);
  }
  printf("======  ==============  ============  ============  ============  "
         "============\n");

  fflush(stdout);
}

/* Custom handling of signals to handle stats and kni processing */
void signal_handler(int signum) {
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
    printf("\nSIGRTMIN/SIGINT/SIGTERM received. "
           "KNI processing stopping.\n");
    __atomic_fetch_add(&kni_stop, 1, __ATOMIC_RELAXED);
    return;
  }
}

int main_loop(__rte_unused void *arg) {
  uint16_t i;
  int32_t f_stop;
  int32_t f_pause;
  const unsigned lcore_id = rte_lcore_id();
  enum lcore_rxtx {
    LCORE_NONE,
    LCORE_ETH_RX,
    LCORE_ETH_TX,
    LCORE_KNI_RX,
    LCORE_KNI_TX,
    LCORE_ETH_XGMII_TX,
    LCORE_ETH_XGMII_RX,
    LCORE_KNI_XGMII_TX,
    LCORE_KNI_XGMII_RX,
    LCORE_VTOP
  };
  enum lcore_rxtx flag = LCORE_NONE;

  RTE_ETH_FOREACH_DEV(i) {
    if (!kni_port_params_array[i])
      continue;
    if (kni_port_params_array[i]->lcore_eth_rx == (uint8_t)lcore_id) {
      flag = LCORE_ETH_RX;
      break;
    } else if (kni_port_params_array[i]->lcore_eth_tx == (uint8_t)lcore_id) {
      flag = LCORE_ETH_TX;
      break;
    } else if (kni_port_params_array[i]->lcore_kni_rx == (uint8_t)lcore_id) {
      flag = LCORE_KNI_RX;
      break;
    } else if (kni_port_params_array[i]->lcore_kni_tx == (uint8_t)lcore_id) {
      flag = LCORE_KNI_TX;
      break;
    } else if (kni_port_params_array[i]->lcore_eth_mii_tx ==
               (uint8_t)lcore_id) {
      flag = LCORE_ETH_XGMII_TX;
      break;
    } else if (kni_port_params_array[i]->lcore_eth_mii_rx ==
               (uint8_t)lcore_id) {
      flag = LCORE_ETH_XGMII_RX;
      break;
    } else if (kni_port_params_array[i]->lcore_kni_mii_tx ==
               (uint8_t)lcore_id) {
      flag = LCORE_KNI_XGMII_TX;
      break;
    } else if (kni_port_params_array[i]->lcore_kni_mii_rx ==
               (uint8_t)lcore_id) {
      flag = LCORE_KNI_XGMII_RX;
      break;
    } else if (kni_port_params_array[i]->lcore_worker_vtop ==
               (uint8_t)lcore_id) {
      flag = LCORE_VTOP;
      break;
    }
  }

  if (flag == LCORE_ETH_RX) {
    RTE_LOG(INFO, APP, "Lcore %u is reading from port %d\n",
            kni_port_params_array[i]->lcore_eth_rx,
            kni_port_params_array[i]->port_id);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      eth_ingress(kni_port_params_array[i], eth_rx_ring);
    }
  } else if (flag == LCORE_ETH_TX) {
    RTE_LOG(INFO, APP, "Lcore %u is writing to port %d\n",
            kni_port_params_array[i]->lcore_eth_tx,
            kni_port_params_array[i]->port_id);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      eth_egress(kni_port_params_array[i], eth_tx_ring);
    }
  } else if (flag == LCORE_KNI_RX) {
    RTE_LOG(INFO, APP, "Lcore %u is reading from KNI\n",
            kni_port_params_array[i]->lcore_kni_rx);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      kni_ingress(kni_port_params_array[i], kni_rx_ring);
    }
  } else if (flag == LCORE_KNI_TX) {
    RTE_LOG(INFO, APP, "Lcore %u is writing to KNI\n",
            kni_port_params_array[i]->lcore_kni_tx);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      kni_egress(kni_port_params_array[i], kni_tx_ring);
    }
  } else if (flag == LCORE_ETH_XGMII_TX) {
    RTE_LOG(INFO, APP, "Lcore %u is converting Ethernet XGMII TX\n",
            kni_port_params_array[i]->lcore_eth_mii_tx);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      xgmii_to_mbuf(kni_stats, &eth_pkt_start, get_vtop_eth_tx_ring(),
                    eth_tx_ring, pktmbuf_pool);
    }
  } else if (flag == LCORE_ETH_XGMII_RX) {
    RTE_LOG(INFO, APP, "Lcore %u is converting Ethernet XGMII RX\n",
            kni_port_params_array[i]->lcore_eth_mii_rx);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      mbuf_to_xgmii(kni_stats, eth_rx_ring, get_vtop_eth_rx_ring(),
                    xgmii_pool);
    }
  } else if (flag == LCORE_KNI_XGMII_TX) {
    RTE_LOG(INFO, APP, "Lcore %u is converting PCIe XGMII TX\n",
            kni_port_params_array[i]->lcore_kni_mii_tx);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      xgmii_to_mbuf(kni_stats, &pci_pkt_start, get_vtop_pci_tx_ring(),
                    kni_tx_ring, pktmbuf_pool);
    }
  } else if (flag == LCORE_KNI_XGMII_RX) {
    RTE_LOG(INFO, APP, "Lcore %u is converting PCIe XGMII RX\n",
            kni_port_params_array[i]->lcore_kni_mii_rx);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      mbuf_to_xgmii(kni_stats, kni_rx_ring, get_vtop_pci_rx_ring(),
                    xgmii_pool);
    }
  } else if (flag == LCORE_VTOP) {
    RTE_LOG(INFO, APP, "Lcore %u is running Verilator sim\n",
            kni_port_params_array[i]->lcore_worker_vtop);
    while (1) {
      f_stop = __atomic_load_n(&kni_stop, __ATOMIC_RELAXED);
      f_pause = __atomic_load_n(&kni_pause, __ATOMIC_RELAXED);
      if (f_stop)
        break;
      if (f_pause)
        continue;
      verilator_top_worker();
    }
  } else
    RTE_LOG(INFO, APP, "Lcore %u has nothing to do\n", lcore_id);

  return 0;
}

/* Display usage instructions */
void print_usage(const char *prgname) {
  RTE_LOG(INFO, APP,
          "\nUsage: %s [EAL options] -- -p PORTMASK -P -m "
          "[--config (port,lcore_rx,lcore_tx,lcore_kthread...)"
          "[,(port,lcore_rx,lcore_tx,lcore_kthread...)]]\n"
          "    -p PORTMASK: hex bitmask of ports to use\n"
          "    -P : enable promiscuous mode\n"
          "    -m : enable monitoring of port carrier state\n"
          "    --config (port,lcore_rx,lcore_tx,lcore_kthread...): "
          "port and lcore configurations\n",
          prgname);
}

/* Convert string to unsigned number. 0 is returned if error occurs */
uint32_t parse_unsigned(const char *portmask) {
  char *end = NULL;
  unsigned long num;

  num = strtoul(portmask, &end, 16);
  if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
    return 0;

  return (uint32_t)num;
}

void print_config(void) {
  uint32_t i, j;
  struct kni_port_params **p = kni_port_params_array;

  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (!p[i])
      continue;
    RTE_LOG(DEBUG, APP, "Port ID: %d\n", p[i]->port_id);
    RTE_LOG(DEBUG, APP, "Eth Rx lcore ID: %u, Eth Tx lcore ID: %u\n",
            p[i]->lcore_eth_rx, p[i]->lcore_eth_tx);
    RTE_LOG(DEBUG, APP, "Kni Rx lcore ID: %u, Kni Tx lcore ID: %u\n",
            p[i]->lcore_kni_rx, p[i]->lcore_kni_tx);
    RTE_LOG(DEBUG, APP, "Eth MII Rx lcore ID: %u, Eth MII Tx lcore ID: %u\n",
            p[i]->lcore_eth_mii_rx, p[i]->lcore_eth_mii_tx);
    RTE_LOG(DEBUG, APP, "PCI MII Rx lcore ID: %u, PCI MII Tx lcore ID: %u\n",
            p[i]->lcore_kni_mii_rx, p[i]->lcore_kni_mii_tx);
    RTE_LOG(DEBUG, APP, "Vtop lcore ID: %d\n", p[i]->lcore_worker_vtop);
    for (j = 0; j < p[i]->nb_lcore_k; j++)
      RTE_LOG(DEBUG, APP, "Kernel thread lcore ID: %u\n", p[i]->lcore_k[j]);
  }
}

int parse_config(const char *arg) {
  const char *p, *p0 = arg;
  char s[256], *end;
  unsigned size;
  enum fieldnames {
    FLD_PORT = 0,
    FLD_LCORE_RX,
    FLD_LCORE_TX,
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
    if ((p0 = strchr(p, ')')) == NULL)
      goto fail;
    size = p0 - p;
    if (size >= sizeof(s)) {
      printf("Invalid config parameters\n");
      goto fail;
    }
    snprintf(s, sizeof(s), "%.*s", size, p);
    nb_token = rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',');
    if (nb_token <= FLD_LCORE_TX) {
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
    kni_port_params_array[port_id]->lcore_eth_rx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_eth_tx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_kni_rx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_kni_tx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_eth_mii_rx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_eth_mii_tx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_kni_mii_rx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_kni_mii_tx = (uint8_t)int_fld[i++];
    kni_port_params_array[port_id]->lcore_worker_vtop = (uint8_t)int_fld[i++];
    if (kni_port_params_array[port_id]->lcore_kni_mii_tx >= RTE_MAX_LCORE ||
        kni_port_params_array[port_id]->lcore_worker_vtop >= RTE_MAX_LCORE) {
      printf("lcore_eth_rx %u or lcore_eth_tx %u ID could not "
             "exceed the maximum %u\n",
             kni_port_params_array[port_id]->lcore_eth_rx,
             kni_port_params_array[port_id]->lcore_eth_tx,
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

int validate_parameters(uint32_t portmask) {
  uint32_t i;

  if (!portmask) {
    printf("No port configured in port mask\n");
    return -1;
  }

  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (((portmask & (1 << i)) && !kni_port_params_array[i]) ||
        (!(portmask & (1 << i)) && kni_port_params_array[i]))
      rte_exit(EXIT_FAILURE, "portmask is not consistent "
                             "to port ids specified in --config\n");

    if (kni_port_params_array[i] &&
        !rte_lcore_is_enabled(
            (unsigned)(kni_port_params_array[i]->lcore_eth_rx)))
      rte_exit(EXIT_FAILURE,
               "lcore id %u for "
               "port %d receiving not enabled\n",
               kni_port_params_array[i]->lcore_eth_rx,
               kni_port_params_array[i]->port_id);

    if (kni_port_params_array[i] &&
        !rte_lcore_is_enabled(
            (unsigned)(kni_port_params_array[i]->lcore_eth_tx)))
      rte_exit(EXIT_FAILURE,
               "lcore id %u for "
               "port %d transmitting not enabled\n",
               kni_port_params_array[i]->lcore_eth_tx,
               kni_port_params_array[i]->port_id);
  }

  return 0;
}

#define CMDLINE_OPT_CONFIG "config"

/* Parse the arguments given in the command line of the application */
int parse_args(int argc, char **argv) {
  int opt, longindex, ret = 0;
  const char *prgname = argv[0];
  struct option longopts[] = {{CMDLINE_OPT_CONFIG, required_argument, NULL, 0},
                              {NULL, 0, NULL, 0}};

  /* Disable printing messages within getopt() */
  opterr = 0;

  /* Parse command line */
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

  /* Check that options were parsed ok */
  if (validate_parameters(ports_mask) < 0) {
    print_usage(prgname);
    rte_exit(EXIT_FAILURE, "Invalid parameters\n");
  }

  return ret;
}

/* Initialize KNI subsystem */
void init_kni(void) {
  unsigned int num_of_kni_ports = 0, i;
  struct kni_port_params **params = kni_port_params_array;

  /* Calculate the maximum number of KNI interfaces that will be used */
  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (kni_port_params_array[i]) {
      num_of_kni_ports += (params[i]->nb_lcore_k ? params[i]->nb_lcore_k : 1);
    }
  }

  /* Invoke rte KNI init to preallocate the ports */
  rte_kni_init(num_of_kni_ports);
}

/* Initialise a single port on an Ethernet device */
void init_port(uint16_t port) {
  int ret;
  uint16_t nb_rxd = NB_RXD;
  uint16_t nb_txd = NB_TXD;
  struct rte_eth_dev_info dev_info;
  struct rte_eth_rxconf rxq_conf;
  struct rte_eth_txconf txq_conf;
  struct rte_eth_conf local_port_conf = port_conf;

  /* Initialise device and RX/TX queues */
  RTE_LOG(INFO, APP, "Initialising port %u ...\n", (unsigned)port);
  fflush(stdout);

  ret = rte_eth_dev_info_get(port, &dev_info);
  if (ret != 0)
    rte_exit(EXIT_FAILURE, "Error during getting device (port %u) info: %s\n",
             port, strerror(-ret));

  if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
    local_port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
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

/* Check the link status of all ports in up to 9s, and print them finally */
void check_all_ports_link_status(uint32_t port_mask) {
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90  /* 9s (90 * 100ms) in total */
  uint16_t portid;
  uint8_t count, all_ports_up, print_flag = 0;
  struct rte_eth_link link;
  int ret;
  char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

  printf("\nChecking link status\n");
  fflush(stdout);
  for (count = 0; count <= MAX_CHECK_TIME; count++) {
    all_ports_up = 1;
    RTE_ETH_FOREACH_DEV(portid) {
      if ((port_mask & (1 << portid)) == 0)
        continue;
      memset(&link, 0, sizeof(link));
      ret = rte_eth_link_get_nowait(portid, &link);
      if (ret < 0) {
        all_ports_up = 0;
        if (print_flag == 1)
          printf("Port %u link get failed: %s\n", portid, rte_strerror(-ret));
        continue;
      }
      /* print link status if flag set */
      if (print_flag == 1) {
        rte_eth_link_to_str(link_status_text, sizeof(link_status_text), &link);
        printf("Port %d %s\n", portid, link_status_text);
        continue;
      }
      /* clear all_ports_up flag if any link down */
      if (link.link_status == RTE_ETH_LINK_DOWN) {
        all_ports_up = 0;
        break;
      }
    }
    /* after finally printing all link status, get out */
    if (print_flag == 1)
      break;

    if (all_ports_up == 0) {
      printf(".");
      fflush(stdout);
      rte_delay_ms(CHECK_INTERVAL);
    }

    /* set the print_flag if all ports up or timeout */
    if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
      print_flag = 1;
      printf("done\n");
    }
  }
}

void log_link_state(struct rte_kni *kni, int prev, struct rte_eth_link *link) {
  char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];
  if (kni == NULL || link == NULL)
    return;

  rte_eth_link_to_str(link_status_text, sizeof(link_status_text), link);
  if (prev != link->link_status)
    RTE_LOG(INFO, APP, "%s NIC %s", rte_kni_get_name(kni), link_status_text);
}

/*
 * Monitor the link status of all ports and update the
 * corresponding KNI interface(s)
 */
void *monitor_all_ports_link_status(void *arg) {
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
      if ((ports_mask & (1 << portid)) == 0)
        continue;
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

int kni_change_mtu_(uint16_t port_id, unsigned int new_mtu) {
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

/* Callback for request of changing MTU */
int kni_change_mtu(uint16_t port_id, unsigned int new_mtu) {
  int ret;

  __atomic_fetch_add(&kni_pause, 1, __ATOMIC_RELAXED);
  ret = kni_change_mtu_(port_id, new_mtu);
  __atomic_fetch_sub(&kni_pause, 1, __ATOMIC_RELAXED);

  return ret;
}

/* Callback for request of configuring network interface up/down */
int kni_config_network_interface(uint16_t port_id, uint8_t if_up) {
  int ret = 0;

  if (!rte_eth_dev_is_valid_port(port_id)) {
    RTE_LOG(ERR, APP, "Invalid port id %d\n", port_id);
    return -EINVAL;
  }

  RTE_LOG(INFO, APP, "Configure network interface of %d %s\n", port_id,
          if_up ? "up" : "down");

  __atomic_fetch_add(&kni_pause, 1, __ATOMIC_RELAXED);

  if (if_up != 0) { /* Configure network interface up */
    ret = rte_eth_dev_stop(port_id);
    if (ret != 0) {
      RTE_LOG(ERR, APP, "Failed to stop port %d: %s\n", port_id,
              rte_strerror(-ret));
      __atomic_fetch_sub(&kni_pause, 1, __ATOMIC_RELAXED);
      return ret;
    }
    ret = rte_eth_dev_start(port_id);
  } else { /* Configure network interface down */
    ret = rte_eth_dev_stop(port_id);
    if (ret != 0) {
      RTE_LOG(ERR, APP, "Failed to stop port %d: %s\n", port_id,
              rte_strerror(-ret));
      __atomic_fetch_sub(&kni_pause, 1, __ATOMIC_RELAXED);
      return ret;
    }
  }

  __atomic_fetch_sub(&kni_pause, 1, __ATOMIC_RELAXED);

  if (ret < 0)
    RTE_LOG(ERR, APP, "Failed to start port %d\n", port_id);

  return ret;
}

void print_ethaddr(const char *name, struct rte_ether_addr *mac_addr) {
  char buf[RTE_ETHER_ADDR_FMT_SIZE];
  rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, mac_addr);
  RTE_LOG(INFO, APP, "\t%s%s\n", name, buf);
}

/* Callback for request of configuring mac address */
int kni_config_mac_address(uint16_t port_id, uint8_t mac_addr[]) {
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

int kni_alloc(uint16_t port_id) {
  uint8_t i;
  struct rte_kni *kni;
  struct rte_kni_conf conf;
  struct kni_port_params **params = kni_port_params_array;
  int ret;

  if (port_id >= RTE_MAX_ETHPORTS || !params[port_id])
    return -1;

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
     * is the main, for multiple kernel thread
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

void init_worker_buffers() {
  // Generate TX and RX queues for pkt_mbufs
  eth_tx_ring = rte_ring_create("eth ring TX", PKT_BURST_SZ, rte_socket_id(),
                                RING_F_SP_ENQ | RING_F_SC_DEQ);
  eth_rx_ring = rte_ring_create("eth ring RX", PKT_BURST_SZ, rte_socket_id(),
                                RING_F_SP_ENQ | RING_F_SC_DEQ);
  kni_tx_ring = rte_ring_create("kni ring TX", PKT_BURST_SZ, rte_socket_id(),
                                RING_F_SP_ENQ | RING_F_SC_DEQ);
  kni_rx_ring = rte_ring_create("kni ring RX", PKT_BURST_SZ, rte_socket_id(),
                                RING_F_SP_ENQ | RING_F_SC_DEQ);
}

void free_worker_buffers() {
  // Generate TX and RX queues for pkt_mbufs
  rte_ring_free(eth_tx_ring);
  rte_ring_free(eth_rx_ring);
  rte_ring_free(kni_tx_ring);
  rte_ring_free(kni_rx_ring);
}

int kni_free_kni(uint16_t port_id) {
  uint8_t i;
  int ret;
  struct kni_port_params **p = kni_port_params_array;

  if (port_id >= RTE_MAX_ETHPORTS || !p[port_id])
    return -1;

  for (i = 0; i < p[port_id]->nb_kni; i++) {
    if (rte_kni_release(p[port_id]->kni[i]))
      printf("Fail to release kni\n");
    p[port_id]->kni[i] = NULL;
  }
  ret = rte_eth_dev_stop(port_id);
  if (ret != 0)
    RTE_LOG(ERR, APP, "Failed to stop port %d: %s\n", port_id,
            rte_strerror(-ret));
  rte_eth_dev_close(port_id);

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

  /* Associate signal_handler function with USR signals */
  signal(SIGUSR1, signal_handler);
  signal(SIGUSR2, signal_handler);
  signal(SIGRTMIN, signal_handler);
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* Initialise EAL */
  ret = rte_eal_init(argc, argv);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Could not initialise EAL (%d)\n", ret);
  argc -= ret;
  argv += ret;

  /* Parse application arguments (after the EAL ones) */
  ret = parse_args(argc, argv);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Could not parse input parameters\n");

  /* Create the mbuf pool */
  pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF, MEMPOOL_CACHE_SZ, 0, MBUF_DATA_SZ, rte_socket_id());
  if (pktmbuf_pool == NULL) {
    rte_exit(EXIT_FAILURE, "Could not initialise mbuf pool\n");
    return -1;
  }

  /* Create the mii frame pool */
  xgmii_pool = rte_pktmbuf_pool_create("mii_pool", XGMII_NB_MBUF, MEMPOOL_CACHE_SZ, 0, XGMII_MBUF_SZ, rte_socket_id());
  if (xgmii_pool == NULL) {
    rte_exit(EXIT_FAILURE, "Could not initialise mbuf pool\n");
    return -1;
  }

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

  /* Initialize Verilated module and tranlation */
  init_worker_buffers();
  init_verilated_top(pktmbuf_pool);

  /* Initialize KNI subsystem */
  init_kni();

  /* Initialise each port */
  RTE_ETH_FOREACH_DEV(port) {
    /* Skip ports that are not enabled */
    if (!(ports_mask & (1 << port)))
      continue;
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
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Could not create link status thread!\n");

  /* Launch per-lcore function on every lcore */
  rte_eal_mp_remote_launch(main_loop, NULL, CALL_MAIN);
  RTE_LCORE_FOREACH_WORKER(i) {
    if (rte_eal_wait_lcore(i) < 0)
      return -1;
  }
  monitor_links = 0;
  pthread_join(kni_link_tid, &retval);

  /* Release resources */
  stop_verilated_top();
  free_worker_buffers();
  RTE_ETH_FOREACH_DEV(port) {
    if (!(ports_mask & (1 << port)))
      continue;
    kni_free_kni(port);
  }
  for (i = 0; i < RTE_MAX_ETHPORTS; i++)
    if (kni_port_params_array[i]) {
      rte_free(kni_port_params_array[i]);
      kni_port_params_array[i] = NULL;
    }

  /* clean up the EAL */
  rte_eal_cleanup();

  return 0;
}
