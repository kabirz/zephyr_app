# SPDX-License-Identifier: Apache-2.0

board_runner_args(probe-rs "--chip" "STM32F103C8")
board_runner_args(pyocd "--target" "STM32F103C8")

include(${ZEPHYR_BASE}/boards/common/probe-rs.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
