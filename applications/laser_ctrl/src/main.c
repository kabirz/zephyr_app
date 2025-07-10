/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <laser-flash.h>
#include <laser-can.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

atomic_t laser_status = ATOMIC_INIT(0);

int main(void)
{
	int can_check_time = 0;
	LOG_INF("build time: %s-%s, board: %s, system clk: %dMHz, version: %s", __DATE__, __TIME__,
		CONFIG_BOARD_TARGET, CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000,
		APP_VERSION_STRING);
	while (true) {
		if (can_check_time < 10) {
			if (check_can_device_ready()) {
				laser_can_init();
				laser_flash_read_mode();
				can_check_time = 10;
			} else {
				can_check_time += 1;
				if (can_check_time >= 10)
					LOG_ERR("can device isn't ready!");
			}
		}
		k_sleep(K_SECONDS(3));
	}
	return 0;
}
