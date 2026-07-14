# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gateway)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/main.c
  src/rf24.c
  src/can_forward.c
  src/udp_forward.c
  src/web_server.c
  src/config.c
  src/persist.c
  src/fw_upgrade.c
)

# HTTP server linker section for resource descriptors
zephyr_linker_sources(SECTIONS src/gateway_http.ld)

# Embed gzipped web page into firmware
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)
generate_inc_file_for_target(app src/web_page.html ${gen_dir}/web_page.html.gz.inc --gzip)
