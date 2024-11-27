# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

target_sources(app PRIVATE
  function.c
  dio.c
  init.c
  history.c
)

target_sources_ifdef(CONFIG_NET_TCP app PRIVATE tcp.c)
target_sources_ifdef(CONFIG_NET_UDP app PRIVATE udp.c)
target_sources_ifdef(CONFIG_DT_HAS_ZEPHYR_MODBUS_SERIAL_ENABLED app PRIVATE rtu.c)
target_sources_ifdef(CONFIG_ADC app PRIVATE adc.c)

