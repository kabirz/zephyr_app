/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP Web 服务器 - 使用 Zephyr HTTP Server API
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/socket.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/sys/reboot.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_web, LOG_LEVEL_INF);

/* ================================================================
 * Web 页面 (gzip 压缩, 编译时生成)
 * ================================================================ */
static const uint8_t web_page_gz[] = {
#include <web_page.html.gz.inc>
};

static struct http_resource_detail_static web_page_resource_detail = {
	.common = {
			.type = HTTP_RESOURCE_TYPE_STATIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
			.content_encoding = "gzip",
			.content_type = "text/html",
		},
	.static_data = web_page_gz,
	.static_data_len = sizeof(web_page_gz),
};

/* ================================================================
 * 固件升级状态
 * ================================================================ */
static struct flash_img_context flash_ctx;

#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)

/* ================================================================
 * API: GET /api/config
 * ================================================================ */
static int get_config_handler(struct http_client_ctx *client,
			      enum http_transaction_status status,
			      const struct http_request_ctx *request_ctx,
			      struct http_response_ctx *response_ctx,
			      void *user_data)
{
	static char json_buf[384];

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	char addr_str[RF24_ADDR_LEN * 2 + 1] = {0};

	for (int i = 0; i < RF24_ADDR_LEN; i++) {
		snprintf(addr_str + i * 2, 3, "%02x", gw_params.rf24_addr[i]);
	}

	int len = snprintf(json_buf, sizeof(json_buf),
		"{\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"udp_port\":%d,"
		"\"rf24_ch\":%d,\"rf24_addr\":\"%s\"}",
		gw_params.ip_addr, gw_params.netmask, gw_params.gateway,
		gw_params.udp_port, gw_params.rf24_channel, addr_str);

	response_ctx->body = json_buf;
	response_ctx->body_len = len;
	response_ctx->final_chunk = true;

	return 0;
}

static struct http_resource_detail_dynamic get_config_resource_detail = {
	.common = {
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		},
	.cb = get_config_handler,
};

/* ================================================================
 * API: POST /api/config
 * ================================================================ */
static int set_config_handler(struct http_client_ctx *client,
			      enum http_transaction_status status,
			      const struct http_request_ctx *request_ctx,
			      struct http_response_ctx *response_ctx,
			      void *user_data)
{
	static uint8_t post_buf[512];
	static size_t cursor;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		cursor = 0;
		return 0;
	}

	if (request_ctx->data_len + cursor > sizeof(post_buf)) {
		cursor = 0;
		return -ENOMEM;
	}

	memcpy(post_buf + cursor, request_ctx->data, request_ctx->data_len);
	cursor += request_ctx->data_len;

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		const char *body = (const char *)post_buf;
		const char *p;

		/* 解析 rf24_ch */
		p = strstr(body, "\"rf24_ch\"");

		if (p) {
			p = strchr(p + 9, ':');
			if (p) {
				int ch = atoi(p + 1);

				if (ch >= 0 && ch <= RF24_ADDR_MAX_CH) {
					gw_params.rf24_channel = (uint8_t)ch;
				}
			}
		}

		/* 解析 udp_port */
		p = strstr(body, "\"udp_port\"");
		if (p) {
			p = strchr(p + 10, ':');
			if (p) {
				int port = atoi(p + 1);

				if (port > 0 && port <= 65535) {
					gw_params.udp_port = (uint16_t)port;
				}
			}
		}

		/* 解析 ip */
		p = strstr(body, "\"ip\"");
		if (p) {
			p = strchr(p + 4, ':');
			if (p && *(p + 1) == '"') {
				p += 2;
				int i = 0;

				while (*p && *p != '"' && i < (int)sizeof(gw_params.ip_addr) - 1) {
					gw_params.ip_addr[i++] = *p++;
				}
				gw_params.ip_addr[i] = '\0';
			}
		}

		/* 解析 netmask */
		p = strstr(body, "\"netmask\"");
		if (p) {
			p = strchr(p + 9, ':');
			if (p && *(p + 1) == '"') {
				p += 2;
				int i = 0;

				while (*p && *p != '"' && i < (int)sizeof(gw_params.netmask) - 1) {
					gw_params.netmask[i++] = *p++;
				}
				gw_params.netmask[i] = '\0';
			}
		}

		/* 解析 gateway */
		p = strstr(body, "\"gateway\"");
		if (p) {
			p = strchr(p + 9, ':');
			if (p && *(p + 1) == '"') {
				p += 2;
				int i = 0;

				while (*p && *p != '"' && i < (int)sizeof(gw_params.gateway) - 1) {
					gw_params.gateway[i++] = *p++;
				}
				gw_params.gateway[i] = '\0';
			}
		}

		/* 持久化并应用 */
		persist_save_rf24_config();
		persist_save_network_config();
		gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);

		LOG_INF("Config updated");

		/* 返回更新后的配置 */
		char addr_str[RF24_ADDR_LEN * 2 + 1] = {0};

		for (int i = 0; i < RF24_ADDR_LEN; i++) {
			snprintf(addr_str + i * 2, 3, "%02x", gw_params.rf24_addr[i]);
		}

		static char resp_buf[384];
		int rlen = snprintf(resp_buf, sizeof(resp_buf),
			"{\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"udp_port\":%d,"
			"\"rf24_ch\":%d,\"rf24_addr\":\"%s\"}",
			gw_params.ip_addr, gw_params.netmask, gw_params.gateway,
			gw_params.udp_port, gw_params.rf24_channel, addr_str);

		response_ctx->body = resp_buf;
		response_ctx->body_len = rlen;
		response_ctx->final_chunk = true;

		cursor = 0;
	}

	return 0;
}

static struct http_resource_detail_dynamic set_config_resource_detail = {
	.common = {
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		},
	.cb = set_config_handler,
};

/* ================================================================
 * API: POST /api/firmware
 * ================================================================ */
static int fw_upgrade_handler(struct http_client_ctx *client,
			      enum http_transaction_status status,
			      const struct http_request_ctx *request_ctx,
			      struct http_response_ctx *response_ctx,
			      void *user_data)
{
	static bool initialized = false;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}

	/* 首次调用: 擦除 Flash 并初始化 */
	if (!initialized) {
		const struct flash_area *fa;
		int ret = flash_area_open(SLOT1_PARTITION_ID, &fa);

		if (ret != 0) {
			LOG_ERR("flash_area_open failed: %d", ret);
			response_ctx->body = "{\"error\":\"flash open failed\"}";
			response_ctx->body_len = strlen(response_ctx->body);
			response_ctx->final_chunk = true;
			return 0;
		}
		flash_area_erase(fa, 0, fa->fa_size);
		flash_area_close(fa);

		ret = flash_img_init(&flash_ctx);
		if (ret != 0) {
			LOG_ERR("flash_img_init failed: %d", ret);
			response_ctx->body = "{\"error\":\"flash init failed\"}";
			response_ctx->body_len = strlen(response_ctx->body);
			response_ctx->final_chunk = true;
			return 0;
		}
		initialized = true;
		LOG_INF("FW upgrade started");
	}

	/* 写入数据 */
	if (request_ctx->data_len > 0) {
		int ret = flash_img_buffered_write(&flash_ctx,
			request_ctx->data, request_ctx->data_len, false);
		if (ret != 0) {
			LOG_ERR("flash write failed: %d", ret);
			initialized = false;
			response_ctx->body = "{\"error\":\"write failed\"}";
			response_ctx->body_len = strlen(response_ctx->body);
			response_ctx->final_chunk = true;
			return 0;
		}
	}

	/* 最后一包: 完成写入并重启 */
	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		flash_img_buffered_write(&flash_ctx, NULL, 0, true);
		initialized = false;
		LOG_INF("FW upgrade complete, rebooting...");

		response_ctx->body = "{\"status\":\"ok\",\"reboot\":true}";
		response_ctx->body_len = strlen(response_ctx->body);
		response_ctx->final_chunk = true;

		k_msleep(1000);
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}

static struct http_resource_detail_dynamic fw_upgrade_resource_detail = {
	.common = {
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		},
	.cb = fw_upgrade_handler,
};

/* ================================================================
 * API: POST /api/reboot
 * ================================================================ */
static int reboot_handler(struct http_client_ctx *client,
			  enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx,
			  void *user_data)
{
	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		response_ctx->body = "{\"status\":\"rebooting\"}";
		response_ctx->body_len = strlen(response_ctx->body);
		response_ctx->final_chunk = true;

		k_msleep(500);
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}

static struct http_resource_detail_dynamic reboot_resource_detail = {
	.common = {
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		},
	.cb = reboot_handler,
};

/* ================================================================
 * HTTP Service 定义
 * ================================================================ */
static uint16_t http_port = 80;

HTTP_SERVICE_DEFINE(http_service, NULL, &http_port,
		    CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(web_page_resource, http_service, "/",
		     &web_page_resource_detail);
HTTP_RESOURCE_DEFINE(web_page_resource2, http_service, "/index.html",
		     &web_page_resource_detail);
HTTP_RESOURCE_DEFINE(get_config_resource, http_service, "/api/config",
		     &get_config_resource_detail);
HTTP_RESOURCE_DEFINE(set_config_resource, http_service, "/api/config",
		     &set_config_resource_detail);
HTTP_RESOURCE_DEFINE(fw_upgrade_resource, http_service, "/api/firmware",
		     &fw_upgrade_resource_detail);
HTTP_RESOURCE_DEFINE(reboot_resource, http_service, "/api/reboot",
		     &reboot_resource_detail);

/* ================================================================
 * 启动
 * ================================================================ */
void gw_web_server_init(void)
{
	int ret = http_server_start();

	if (ret < 0) {
		LOG_ERR("HTTP server start failed: %d", ret);
	} else {
		LOG_INF("HTTP server started on port %d", http_port);
	}
}
