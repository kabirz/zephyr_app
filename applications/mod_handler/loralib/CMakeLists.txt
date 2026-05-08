cmake_minimum_required(VERSION 3.15)
project(LoRaGatewaySDK C)

set(CMAKE_C_STANDARD 11)

# --- LoRa Gateway SDK DLL ---
add_library(lora_gateway_sdk SHARED
    lora_sdk.c
    lora_sdk_net.c
    lora_sdk_tcp.c
    lora_sdk_udp.c
    crc16.c
    cJSON.c
)

target_compile_definitions(lora_gateway_sdk PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(lora_gateway_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lora_gateway_sdk PRIVATE
    ws2_32
    iphlpapi
)

if(MSVC)
    target_compile_options(lora_gateway_sdk PRIVATE /W3 /utf-8)
endif()

# --- Example (optional) ---
add_subdirectory(examples)
