# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(SNIPPET
  shell_en
  littlefs_flash
  imgmgr
  ftp_server
)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(data-collect)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_RTC app PRIVATE src/time.c)
target_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM app PRIVATE src/fs.c)

add_subdirectory_ifdef(CONFIG_MODBUS src/modbus)
add_subdirectory_ifdef(CONFIG_HTTP_SERVER src/http_server)
add_subdirectory_ifdef(CONFIG_FTP_SERVER src/ftp_server)

