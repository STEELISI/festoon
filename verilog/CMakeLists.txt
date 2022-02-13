project(verilated_model)
cmake_minimum_required(VERSION 3.16)

add_compile_options(-O3 -flto)

find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})

add_library(Vtop)
verilate(Vtop
    THREADS OPT_SLOW
    VERILATOR_ARGS -Wno-fatal
    SOURCES
      verilog/top.v
    INCLUDE_DIRS
      verilog/
    TOP_MODULE top
)
