
if (CONFIG_ETH_RESET_ENABLE)
  zephyr_library()
  zephyr_library_sources(board.c)
endif()
