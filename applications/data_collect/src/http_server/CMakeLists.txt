# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

zephyr_linker_sources(SECTIONS http_server.ld)
zephyr_linker_section(NAME
	http_resource_desc_test_http_service
	KVMA RAM_REGION GROUP RODATA_REGION
	SUBALIGN ${CONFIG_LINKER_ITERABLE_SUBALIGN}
)

target_sources(app PRIVATE http_server.c)

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)

foreach(web_resource index.html)
  generate_inc_file_for_target(
    app
    resources/${web_resource}
    ${gen_dir}/${web_resource}.gz.inc
    --gzip
  )
endforeach()

