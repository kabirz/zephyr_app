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
#include <display.h>
#include <power.h>

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

#define ACTIVITY_TIMEOUT_MS (10 * 60 * 1000) /* 10 minutes */

volatile uint32_t last_activity_time;

int main(void)
{
	LOG_INF("build time: %s-%s", __DATE__, __TIME__);
	LOG_INF("board: %s, system clk: %dMHz", CONFIG_BOARD_TARGET,
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / MHZ(1));
	LOG_INF("flash size: %dKB, ram size: %dKB", CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE);
	LOG_INF("version: %s", APP_VERSION_STRING);

	last_activity_time = k_uptime_get_32();

	while (1) {
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}

		if ((k_uptime_get_32() - last_activity_time) > ACTIVITY_TIMEOUT_MS) {
			k_event_clear(&global_params.event, WAKE_EVENT);
			global_params.sleeping = true;
			can_power_enable(false);
			lora_power_enable(false);
			dis_power_enable(false);
			p5_power_enable(false);
			LOG_INF("system entering sleep (inactivity timeout)");
			continue;
		}

		k_sleep(K_MSEC(500));
	}

	return 0;
}
