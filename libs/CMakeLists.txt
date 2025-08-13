
cmake_minimum_required(VERSION 3.13.1)

if (CONFIG_HW_TEST)
zephyr_sources(
  LCD/lcd.c
  24C02/24c02.c
)

zephyr_sources_ifdef(CONFIG_SHELL lib_shell.c)
endif()
