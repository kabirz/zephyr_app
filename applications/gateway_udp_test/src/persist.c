/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings 持久化存储 — 使用 Zephyr settings 子系统 (FCB 后端, cfg_partition)
 * 平移自 gateway/src/persist.c, 去掉 RF24 字段.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <gateway_udp_test.h>

LOG_MODULE_REGISTER(gut_persist, LOG_LEVEL_INF);

static int gut_persist_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	size_t name_len = settings_name_next(name, &next);

	if (!next && !strncmp(name, "ip_addr", name_len)) {
		if (len < sizeof(gut_params.ip_addr)) {
			read_cb(cb_arg, gut_params.ip_addr, len);
			gut_params.ip_addr[len] = '\0';
		}
		return 0;
	}

	if (!next && !strncmp(name, "netmask", name_len)) {
		if (len < sizeof(gut_params.netmask)) {
			read_cb(cb_arg, gut_params.netmask, len);
			gut_params.netmask[len] = '\0';
		}
		return 0;
	}

	if (!next && !strncmp(name, "gateway", name_len)) {
		if (len < sizeof(gut_params.gateway)) {
			read_cb(cb_arg, gut_params.gateway, len);
			gut_params.gateway[len] = '\0';
		}
		return 0;
	}

	if (!next && !strncmp(name, "data_port", name_len)) {
		if (len == sizeof(uint16_t)) {
			read_cb(cb_arg, &gut_params.data_port, sizeof(uint16_t));
		}
		return 0;
	}

	return -ENOENT;
}

static int gut_persist_export(int (*cb)(const char *name, const void *value, size_t val_len))
{
	(void)cb("gut/ip_addr", gut_params.ip_addr, strlen(gut_params.ip_addr));
	(void)cb("gut/netmask", gut_params.netmask, strlen(gut_params.netmask));
	(void)cb("gut/gateway", gut_params.gateway, strlen(gut_params.gateway));
	(void)cb("gut/data_port", &gut_params.data_port, sizeof(gut_params.data_port));
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(gut_persist, "gut", NULL, gut_persist_set, NULL,
			       gut_persist_export);

static int settings_backend_init(void)
{
	int rc = settings_subsys_init();

	if (rc) {
		LOG_ERR("settings_subsys_init failed: %d", rc);
	} else {
		LOG_INF("Settings subsystem initialized");
	}
	return rc;
}

void persist_save_network_config(void)
{
	settings_save_one("gut/ip_addr", gut_params.ip_addr, strlen(gut_params.ip_addr));
	settings_save_one("gut/netmask", gut_params.netmask, strlen(gut_params.netmask));
	settings_save_one("gut/gateway", gut_params.gateway, strlen(gut_params.gateway));
	settings_save_one("gut/data_port", &gut_params.data_port, sizeof(gut_params.data_port));
	LOG_INF("Saved network: ip=%s port=%d", gut_params.ip_addr, gut_params.data_port);
}

SYS_INIT(settings_backend_init, APPLICATION, 10);
