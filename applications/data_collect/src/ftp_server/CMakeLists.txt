# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

target_sources(app PRIVATE
  ftpd.c
  ftp_cmds.c
  ftp_handler.c
)

set_source_files_properties(
  ftp_cmds.c
  TARGET_DIRECTORY app
  PROPERTIES COMPILE_FLAGS -Wno-format-truncation
)

