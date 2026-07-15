/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 透传模块 - nRF24/CAN 与上位机之间的双向 UDP 转发
 * + 网络配置 + 固件升级 (通过 UDP)
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <errno.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_udp, LOG_LEVEL_INF);

#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)

/* UDP 命令协议 - 使用 2 字节魔数头区分命令和数据帧
 * 命令格式: [0xAA][0x55][cmd 1B][data...]
 * 数据格式: [CAN ID 2B BE][payload]
 */
#define UDP_MAGIC_0 0xAA
#define UDP_MAGIC_1 0x55
#define UDP_HDR_SIZE 3  /* magic(2) + cmd(1) */

enum udp_cmd {
	UDP_CMD_SET_IP = 0x01,
	UDP_CMD_SET_MASK = 0x02,
	UDP_CMD_SET_GW = 0x03,
	UDP_CMD_SET_PORT = 0x04,
	UDP_CMD_GET_CONFIG = 0x05,
	UDP_CMD_SET_MODE = 0x06,
	UDP_CMD_SET_RF24_CH = 0x07,
	UDP_CMD_SET_RF24_ADDR = 0x08,
	UDP_CMD_REBOOT = 0x09,
	UDP_CMD_FW_START = 0x10,
	UDP_CMD_FW_DATA = 0x11,
	UDP_CMD_FW_END = 0x12,
};

static int udp_sock = -1;
static struct sockaddr_in remote_addr;

/* ================================================================
 * 固件升级状态
 * ================================================================ */
static struct flash_img_context flash_img_ctx;
static bool fw_started = false;

static void fw_reset(void)
{
	if (fw_started) {
		flash_img_buffered_write(&flash_img_ctx, NULL, 0, true);
		fw_started = false;
		LOG_INF("FW upgrade complete");
	}
}

/* ================================================================
 * UDP 发送
 * ================================================================ */
void gw_udp_send(const uint8_t *data, size_t len)
{
	if (udp_sock < 0 || len == 0) {
		return;
	}

	sendto(udp_sock, data, len, 0, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
}

static void udp_send_resp(uint8_t cmd, const uint8_t *data, uint8_t len)
{
	uint8_t buf[64] = {0};

	buf[0] = UDP_MAGIC_0;
	buf[1] = UDP_MAGIC_1;
	buf[2] = cmd;
	if (len > 0 && len < sizeof(buf) - UDP_HDR_SIZE) {
		memcpy(buf + UDP_HDR_SIZE, data, len);
	}
	gw_udp_send(buf, len + UDP_HDR_SIZE);
}

/* ================================================================
 * UDP 命令处理
 * ================================================================ */
static void udp_cmd_handler(const uint8_t *data, size_t len)
{
	if (len < UDP_HDR_SIZE) {
		return;
	}

	uint8_t cmd = data[2];
	const uint8_t *cmd_data = data + UDP_HDR_SIZE;
	size_t cmd_len = len - UDP_HDR_SIZE;

	switch (cmd) {
	case UDP_CMD_SET_IP:
		if (cmd_len >= 4) {
			struct in_addr addr;

			addr.s4_addr[0] = cmd_data[0];
			addr.s4_addr[1] = cmd_data[1];
			addr.s4_addr[2] = cmd_data[2];
			addr.s4_addr[3] = cmd_data[3];
			inet_ntop(AF_INET, &addr, gw_params.ip_addr, sizeof(gw_params.ip_addr));
			LOG_INF("UDP set IP: %s", gw_params.ip_addr);
			persist_save_network_config();
			udp_send_resp(cmd, (uint8_t *)gw_params.ip_addr, strlen(gw_params.ip_addr));
		}
		break;

	case UDP_CMD_SET_MASK:
		if (cmd_len >= 4) {
			struct in_addr mask;

			mask.s4_addr[0] = cmd_data[0];
			mask.s4_addr[1] = cmd_data[1];
			mask.s4_addr[2] = cmd_data[2];
			mask.s4_addr[3] = cmd_data[3];
			inet_ntop(AF_INET, &mask, gw_params.netmask, sizeof(gw_params.netmask));
			LOG_INF("UDP set mask: %s", gw_params.netmask);
			persist_save_network_config();
			udp_send_resp(cmd, (uint8_t *)gw_params.netmask, strlen(gw_params.netmask));
		}
		break;

	case UDP_CMD_SET_GW:
		if (cmd_len >= 4) {
			struct in_addr gw;

			gw.s4_addr[0] = cmd_data[0];
			gw.s4_addr[1] = cmd_data[1];
			gw.s4_addr[2] = cmd_data[2];
			gw.s4_addr[3] = cmd_data[3];
			inet_ntop(AF_INET, &gw, gw_params.gateway, sizeof(gw_params.gateway));
			LOG_INF("UDP set gw: %s", gw_params.gateway);
			persist_save_network_config();
			udp_send_resp(cmd, (uint8_t *)gw_params.gateway, strlen(gw_params.gateway));
		}
		break;

	case UDP_CMD_SET_PORT:
		if (cmd_len >= 2) {
			gw_params.udp_port = sys_get_be16(cmd_data);
			LOG_INF("UDP set port: %d", gw_params.udp_port);
			persist_save_network_config();
			udp_send_resp(cmd, cmd_data, 2);
		}
		break;

	case UDP_CMD_GET_CONFIG: {
		uint8_t buf[32] = {0};
		int offset = 0;

		buf[offset++] = gw_params.connect_type;
		buf[offset++] = gw_params.rf24_channel;
		memcpy(buf + offset, gw_params.rf24_addr, RF24_ADDR_LEN);
		offset += RF24_ADDR_LEN;
		sys_put_be16(gw_params.udp_port, buf + offset);
		offset += 2;
		udp_send_resp(cmd, buf, offset);
		break;
	}

	case UDP_CMD_SET_MODE:
		if (cmd_len >= 1) {
			if (cmd_data[0] == GW_MODE_CAN || cmd_data[0] == GW_MODE_UDP) {
				gw_params.connect_type = cmd_data[0];
				LOG_INF("UDP set mode: %s", gw_params.connect_type == GW_MODE_CAN ? "CAN" : "UDP");
				persist_save_network_config();
			}
			udp_send_resp(cmd, &gw_params.connect_type, 1);
		}
		break;

	case UDP_CMD_SET_RF24_CH:
		if (cmd_len >= 1 && cmd_data[0] <= RF24_ADDR_MAX_CH) {
			gw_params.rf24_channel = cmd_data[0];
			persist_save_rf24_config();
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
			LOG_INF("UDP set rf24 ch: %d", gw_params.rf24_channel);
		}
		udp_send_resp(cmd, &gw_params.rf24_channel, 1);
		break;

	case UDP_CMD_SET_RF24_ADDR:
		if (cmd_len >= RF24_ADDR_LEN) {
			memcpy(gw_params.rf24_addr, cmd_data, RF24_ADDR_LEN);
			persist_save_rf24_config();
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
			LOG_INF("UDP set rf24 addr");
		}
		udp_send_resp(cmd, gw_params.rf24_addr, RF24_ADDR_LEN);
		break;

	case UDP_CMD_REBOOT:
		LOG_INF("UDP reboot requested");
		udp_send_resp(cmd, NULL, 0);
		k_msleep(100);
		sys_reboot(SYS_REBOOT_COLD);
		break;

	case UDP_CMD_FW_START:
		if (!fw_started) {
			const struct flash_area *fa;

			if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
				LOG_ERR("flash_area_open failed");
				udp_send_resp(cmd, (uint8_t *)"error", 5);
				return;
			}
			flash_area_erase(fa, 0, fa->fa_size);
			flash_area_close(fa);

			if (flash_img_init(&flash_img_ctx) != 0) {
				LOG_ERR("flash_img_init failed");
				udp_send_resp(cmd, (uint8_t *)"error", 5);
				return;
			}
			fw_started = true;
			LOG_INF("FW upgrade started");
		}
		udp_send_resp(cmd, (uint8_t *)"ok", 2);
		break;

	case UDP_CMD_FW_DATA:
		if (fw_started && cmd_len > 0) {
			if (flash_img_buffered_write(&flash_img_ctx, cmd_data, cmd_len, false) != 0) {
				LOG_ERR("flash write failed");
				fw_started = false;
				udp_send_resp(cmd, (uint8_t *)"error", 5);
			} else {
				udp_send_resp(cmd, (uint8_t *)"ok", 2);
			}
		}
		break;

	case UDP_CMD_FW_END:
		if (fw_started) {
			fw_reset();
			udp_send_resp(cmd, (uint8_t *)"ok", 2);
			LOG_INF("FW upgrade complete, rebooting...");
			k_msleep(500);
			sys_reboot(SYS_REBOOT_COLD);
		}
		break;

	default:
		LOG_DBG("Unknown UDP cmd: 0x%02x", cmd);
		break;
	}
}

/* ================================================================
 * UDP 接收线程
 * ================================================================ */
static void udp_rx_thread(void)
{
	udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udp_sock < 0) {
		LOG_ERR("UDP socket create failed: %d", errno);
		return;
	}

	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(gw_params.udp_port),
	};

	if (bind(udp_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		LOG_ERR("UDP bind failed: %d", errno);
		close(udp_sock);
		udp_sock = -1;
		return;
	}

	LOG_INF("UDP listening on port %d", gw_params.udp_port);

	remote_addr.sin_family = AF_INET;
	remote_addr.sin_port = htons(gw_params.udp_port);
	inet_pton(AF_INET, "255.255.255.255", &remote_addr.sin_addr);

	uint8_t buf[512];

	while (1) {
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);

		ssize_t received = recvfrom(udp_sock, buf, sizeof(buf), 0,
					    (struct sockaddr *)&src_addr, &addr_len);
		if (received <= 0) {
			continue;
		}

		remote_addr = src_addr;

		/* 检查魔数头判断是命令还是数据帧 */
		if (received >= UDP_HDR_SIZE && buf[0] == UDP_MAGIC_0 && buf[1] == UDP_MAGIC_1) {
			udp_cmd_handler(buf, received);
		} else if (received >= 2) {
			/* 透传扫描仪数据到 nRF24 */
			uint16_t can_id = sys_get_be16(buf);

			if (can_id == OVERBREAK_LASER || can_id == COORD_XY || can_id == COORD_Z) {
				gw_rf24_send(can_id, buf + 2, received - 2);
				LOG_DBG("UDP->nRF24: id=0x%03x len=%zd", can_id, received - 2);
			}
		}
	}
}

K_THREAD_DEFINE(thread_udp_rx, CONFIG_GATEWAY_UDP_STACK, udp_rx_thread, NULL, NULL, NULL,
		CONFIG_GATEWAY_UDP_PRIORITY, 0, 0);
