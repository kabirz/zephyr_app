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
#include <display.h>

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

gloval_params_t global_params;

int main(void)
{
	printk("build time: %s-%s, board: %s, system clk: %dMHz, flash size: %dKB, version: %s\n",
	       __DATE__, __TIME__, CONFIG_BOARD_TARGET,
	       CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000, CONFIG_FLASH_SIZE, APP_VERSION_STRING);
	mod_display_init();
	mod_display_clear();
	while (1) {
		mod_display_all(&global_params);
		k_sleep(K_MSEC(500));
	}

	return 0;
}

static int main_init(void)
{
	global_params.can_heart_time = CAN_HEART_TIME;
	global_params.connect_type = CAN_TYPE;
	k_event_init(&global_params.event);
	return 0;
}

SYS_INIT(main_init, APPLICATION, 10);
