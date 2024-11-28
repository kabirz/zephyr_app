# SPDX-License-Identifier: Apache-2.0

# keep first
board_runner_args(openocd --cmd-post-verify "reset halt")
board_runner_args(openocd --target-handle=_CHIPNAME.cpu0)
board_runner_args(pyocd "--target=stm32h743iitx")
board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=hw")

# keep first
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
