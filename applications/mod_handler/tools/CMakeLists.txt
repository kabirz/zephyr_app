cmake_minimum_required(VERSION 3.15)
project(LoRaGatewayTool C)

set(CMAKE_C_STANDARD 11)

# We use A-suffix Win32 APIs explicitly, no UNICODE needed

add_executable(lora_gateway_tool WIN32
    lora_gateway_tool.c
    lora_net.c
    lora_tcp.c
    lora_udp.c
    crc16.c
    cJSON.c
)

target_link_libraries(lora_gateway_tool PRIVATE
    ws2_32
    comctl32
    iphlpapi
)

# MSVC specific warnings
if(MSVC)
    target_compile_options(lora_gateway_tool PRIVATE /W3 /utf-8)
endif()

# MinGW: 源码 UTF-8, 运行时 Win32 A-API 需要 GBK
if(MINGW)
    target_compile_options(lora_gateway_tool PRIVATE -finput-charset=UTF-8 -fexec-charset=GBK)
endif()
