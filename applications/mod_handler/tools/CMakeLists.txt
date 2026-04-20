cmake_minimum_required(VERSION 3.15)
project(LoRaGatewayTool C)

set(CMAKE_C_STANDARD 11)

# We use A-suffix Win32 APIs explicitly, no UNICODE needed
# UTF-8 manifest makes *A APIs accept UTF-8 on Windows 10 1903+

add_executable(lora_gateway_tool WIN32
    lora_gateway_tool.c
    lora_net.c
    lora_tcp.c
    lora_udp.c
    crc16.c
    cJSON.c
    resources/app.rc
)

target_link_libraries(lora_gateway_tool PRIVATE
    ws2_32
    comctl32
    iphlpapi
)

# MSVC: /utf-8 for source + exec charset
if(MSVC)
    target_compile_options(lora_gateway_tool PRIVATE /W3 /utf-8)
endif()
