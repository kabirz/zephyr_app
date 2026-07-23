/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings 持久化存储 - 使用 Zephyr settings 子系统 (FCB 后端)
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_persist, LOG_LEVEL_INF);

static int gw_persist_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	size_t name_len = settings_name_next(name, &next);

	if (!next && !strncmp(name, "rf24_channel", name_len)) {
		if (len == sizeof(uint8_t)) {
			uint8_t ch;

			read_cb(cb_arg, &ch, sizeof(ch));
			if (ch <= RF24_ADDR_MAX_CH) {
				gw_params.rf24_channel = ch;
			}
		}
		return 0;
	}

	if (!next && !strncmp(name, "rf24_addr", name_len)) {
		if (len == RF24_ADDR_LEN) {
			read_cb(cb_arg, gw_params.rf24_addr, RF24_ADDR_LEN);
		}
		return 0;
	}

	if (!next && !strncmp(name, "ip_addr", name_len)) {
		if (len < sizeof(gw_params.ip_addr)) {
			read_cb(cb_arg, gw_params.ip_addr, len);
			gw_params.ip_addr[len] = '\0';
		}
		return 0;
	}

	if (!next && !strncmp(name, "netmask", name_len)) {
		if (len < sizeof(gw_params.netmask)) {
			read_cb(cb_arg, gw_params.netmask, len);
			gw_params.netmask[len] = '\0';
		}
		return 0;
	}

	if (!next && !strncmp(name, "gateway", name_len)) {
		if (len < sizeof(gw_params.gateway)) {
			read_cb(cb_arg, gw_params.gateway, len);
			gw_params.gateway[len] = '\0';
		}
		return 0;
	}

	if (!next && !strncmp(name, "udp_port", name_len)) {
		if (len == sizeof(uint16_t)) {
			read_cb(cb_arg, &gw_params.udp_port, sizeof(uint16_t));
		}
		return 0;
	}

	return -ENOENT;
}

static int gw_persist_export(int (*cb)(const char *name, const void *value, size_t val_len))
{
	(void)cb("gw/rf24_channel", &gw_params.rf24_channel, sizeof(gw_params.rf24_channel));
	(void)cb("gw/rf24_addr", gw_params.rf24_addr, RF24_ADDR_LEN);
	(void)cb("gw/ip_addr", gw_params.ip_addr, strlen(gw_params.ip_addr));
	(void)cb("gw/netmask", gw_params.netmask, strlen(gw_params.netmask));
	(void)cb("gw/gateway", gw_params.gateway, strlen(gw_params.gateway));
	(void)cb("gw/udp_port", &gw_params.udp_port, sizeof(gw_params.udp_port));
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(gw_persist, "gw", NULL, gw_persist_set, NULL, gw_persist_export);

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

void persist_save_rf24_config(void)
{
	settings_save_one("gw/rf24_channel", &gw_params.rf24_channel,
			  sizeof(gw_params.rf24_channel));
	settings_save_one("gw/rf24_addr", gw_params.rf24_addr, RF24_ADDR_LEN);
	LOG_INF("Saved rf24: ch=%d", gw_params.rf24_channel);
}

void persist_save_network_config(void)
{
	settings_save_one("gw/ip_addr", gw_params.ip_addr, strlen(gw_params.ip_addr));
	settings_save_one("gw/netmask", gw_params.netmask, strlen(gw_params.netmask));
	settings_save_one("gw/gateway", gw_params.gateway, strlen(gw_params.gateway));
	settings_save_one("gw/udp_port", &gw_params.udp_port, sizeof(gw_params.udp_port));
	LOG_INF("Saved network: ip=%s port=%d", gw_params.ip_addr, gw_params.udp_port);
}

SYS_INIT(settings_backend_init, APPLICATION, 10);
