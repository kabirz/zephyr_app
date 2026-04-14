# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mod_handler)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/adc.c
  src/battery.c
  src/can.c
  src/display.c
  src/firmware.c
  src/lora.c
  src/main.c
  src/power.c
)

