/*
 * Copyright (c) 2026 Kabirz.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif
#include <zephyr/logging/log.h>
#include <common.h>
#include <mod-can.h>

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

gloval_params_t global_params;

int main(void)
{
	global_params.can_heart_time = CAN_HEART_TIME;
	k_event_init(&global_params.event);

	printk("build time: %s-%s, board: %s, system clk: %dMHz, flash size: %dKB, version: %s\n",
	       __DATE__, __TIME__, CONFIG_BOARD_TARGET,
	       CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000, CONFIG_FLASH_SIZE, APP_VERSION_STRING);

	while (1) {
		k_sleep(K_SECONDS(5));
	}

	return 0;
}
