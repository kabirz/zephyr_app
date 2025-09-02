
if (CONFIG_ETH_RESET_ENABLE)
  zephyr_library()
  zephyr_library_sources(board.c)
endif()
zephyr_linker_sources_ifdef(CONFIG_MEMC_STM32_NOR_PSRAM SECTIONS psram.ld)
