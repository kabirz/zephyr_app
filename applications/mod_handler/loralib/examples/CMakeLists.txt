cmake_minimum_required(VERSION 3.15)
project(lora_sdk_example C)

set(CMAKE_C_STANDARD 11)

add_executable(example WIN32 example.c)

target_link_libraries(example PRIVATE lora_gateway_sdk)
target_include_directories(example PRIVATE ${CMAKE_SOURCE_DIR})
