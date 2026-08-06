# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(SNIPPET
  shell_en
  littlefs_flash
  ftp_server
)
list(APPEND SNIPPET_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(data-collect)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_RTC app PRIVATE src/time.c)
target_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM app PRIVATE src/fs.c)
target_sources_ifdef(CONFIG_NETWORKING app PRIVATE src/net.c)
target_sources_ifdef(CONFIG_CAN_FW_UPGRADE app PRIVATE src/can.c)

add_subdirectory_ifdef(CONFIG_MODBUS src/modbus)
add_subdirectory_ifdef(CONFIG_FTP_SERVER src/ftp_server)

target_compile_options(app PRIVATE -U_POSIX_C_SOURCE -D_POSIX_C_SOURCE=200809L)
