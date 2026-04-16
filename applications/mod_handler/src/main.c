/*
 * Copyright (c) 2026 Kabirz.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/app_version.h>
#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif
#include <zephyr/logging/log.h>
#include <common.h>
#include <mod-can.h>

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);


int main(void)
{
	LOG_INF("build time: %s-%s", __DATE__, __TIME__);
	LOG_INF("board: %s, system clk: %dMHz", CONFIG_BOARD_TARGET,
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / MHZ(1));
	LOG_INF("flash size: %dKB, ram size: %dKB", CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE);
	LOG_INF("version: %s", APP_VERSION_STRING);
	while (1) {
		k_sleep(K_MSEC(500));
	}

	return 0;
}
