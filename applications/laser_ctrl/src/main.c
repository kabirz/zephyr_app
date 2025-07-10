/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

atomic_t laser_status = ATOMIC_INIT(0);

int main(void)
{
	LOG_INF("build time: %s-%s, board: %s, system clk: %dMHz, version: %s", __DATE__, __TIME__,
		CONFIG_BOARD_TARGET, CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000,
		APP_VERSION_STRING);

	return 0;
}
