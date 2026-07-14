
if (CONFIG_HW_TEST)
zephyr_sources(
  LCD/lcd.c
  24C02/24c02.c
)

zephyr_sources_ifdef(CONFIG_SHELL lib_shell.c)
endif()

add_subdirectory_ifdef(CONFIG_CAN_FW_UPGRADE can_fw_upgrade)
