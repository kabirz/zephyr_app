# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# 注册 snippets/ 目录
list(APPEND SNIPPET_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# 可选 snippet: 网关板 W5500 网络
#   启用: WITH_GATEWAY_ETH=1 west build -b nrf24_f103rct6 gateway --sysbuild
# 注: sysbuild 不转发命令行 -D 给子镜像, 改用环境变量在 app 内追加 SNIPPET
if(DEFINED ENV{WITH_GATEWAY_ETH})
    list(APPEND SNIPPET gateway_eth)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gateway)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/main.c
  src/rf24.c
  src/can_forward.c
  src/config.c
  src/persist.c
)

# UDP 透传模块仅在启用网络 (CONFIG_GW_NETWORKING) 时编译
if(CONFIG_GW_NETWORKING)
  target_sources(app PRIVATE src/udp_forward.c)
endif()
