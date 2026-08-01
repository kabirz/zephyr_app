/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings 持久化存储 — 使用 Zephyr settings 子系统 (FCB 后端, cfg_partition)
 * 平移自 gateway/src/persist.c; 保留 RF24 字段用于协议验证 (无硬件, 仅持久化).
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

	if (!next && !strncmp(name, "rf24_channel", name_len)) {
		if (len == sizeof(uint8_t)) {
			uint8_t ch;

			read_cb(cb_arg, &ch, sizeof(ch));
			if (ch <= RF24_ADDR_MAX_CH) {
				gut_params.rf24_channel = ch;
			}
		}
		return 0;
	}

	if (!next && !strncmp(name, "rf24_addr", name_len)) {
		if (len == RF24_ADDR_LEN) {
			read_cb(cb_arg, gut_params.rf24_addr, RF24_ADDR_LEN);
		}
		return 0;
	}

	if (!next && !strncmp(name, "ip_addr", name_len)) {
		if (len < sizeof(gut_params.ip_addr)) {
			read_cb(cb_arg, gut_params.ip_addr, len);
			gut_params.ip_addr[len] = '\0';
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
	(void)cb("gut/rf24_channel", &gut_params.rf24_channel, sizeof(gut_params.rf24_channel));
	(void)cb("gut/rf24_addr", gut_params.rf24_addr, RF24_ADDR_LEN);
	(void)cb("gut/ip_addr", gut_params.ip_addr, strlen(gut_params.ip_addr));
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
		/* 清除旧协议残留的历史键 (掩码/网关已不再持久化).
		 * 不删除的话 settings_load 回放时会打 -ENOENT 错误日志. */
		settings_delete("gut/netmask");
		settings_delete("gut/gateway");
	}
	return rc;
}

void persist_save_rf24_config(void)
{
	settings_save_one("gut/rf24_channel", &gut_params.rf24_channel,
			  sizeof(gut_params.rf24_channel));
	settings_save_one("gut/rf24_addr", gut_params.rf24_addr, RF24_ADDR_LEN);
	LOG_INF("Saved rf24: addr=%02x%02x%02x%02x%02x",
			gut_params.rf24_addr[0], gut_params.rf24_addr[1], gut_params.rf24_addr[2],
			gut_params.rf24_addr[3], gut_params.rf24_addr[4]);
}

void persist_save_network_config(void)
{
	settings_save_one("gut/ip_addr", gut_params.ip_addr, strlen(gut_params.ip_addr));
	settings_save_one("gut/data_port", &gut_params.data_port, sizeof(gut_params.data_port));
	LOG_INF("Saved network: ip=%s port=%d", gut_params.ip_addr, gut_params.data_port);
}

SYS_INIT(settings_backend_init, APPLICATION, 10);
