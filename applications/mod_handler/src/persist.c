/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings 持久化存储 - 使用 Zephyr settings 子系统 + FCB 后端
 * 参考 data_collect 项目的 settings 实现模式
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <common.h>

LOG_MODULE_REGISTER(persist, LOG_LEVEL_INF);

static int persist_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	size_t name_len = settings_name_next(name, &next);

	if (!next && !strncmp(name, "connect_type", name_len)) {
		if (len == sizeof(uint8_t)) {
			uint8_t type;

			read_cb(cb_arg, &type, sizeof(type));
			if (type == CAN_TYPE || type == LORA_TYPE) {
				global_params.connect_type = type;
				LOG_INF("Loaded connect_type: %d (%s)", type,
					type == CAN_TYPE ? "CAN" : "LoRa");
			}
		}
		return 0;
	}

	return -ENOENT;
}

static int persist_export(int (*cb)(const char *name, const void *value, size_t val_len))
{
	(void)cb("persist/connect_type", &global_params.connect_type,
		 sizeof(global_params.connect_type));
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(persist, "persist", NULL, persist_set, NULL, persist_export);

static int settings_backend_init(void)
{
	int rc = settings_subsys_init();

	if (rc) {
		LOG_ERR("settings_subsys_init failed: %d", rc);
	} else {
		LOG_INF("settings subsystem initialized");
	}
	return rc;
}

void persist_save_connect_type(void)
{
	int rc = settings_save_one("persist/connect_type", &global_params.connect_type,
				   sizeof(global_params.connect_type));

	if (rc) {
		LOG_ERR("Failed to save connect_type: %d", rc);
	} else {
		LOG_INF("Saved connect_type: %d", global_params.connect_type);
	}
}

SYS_INIT(settings_backend_init, APPLICATION, 10);
