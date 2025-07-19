# SPDX-License-Identifier: Apache-2.0

board_runner_args(probe-rs "--chip" "STM32F407VETx" "--probe-rs" "probe-rs")

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/probe-rs.board.cmake)
include(${ZEPHYR_BASE}/boards/common/canopen.board.cmake)

