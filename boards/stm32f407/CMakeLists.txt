
if(CONFIG_BOARD_STM32F407_STM32F407XX_XINXIN AND CONFIG_DT_HAS_ST_STM32_ETHERNET_ENABLED)
  zephyr_library()
  zephyr_library_sources(board.c)
endif()
