/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 转发模块 - CAN 总线与 nRF24/UDP 之间的数据中转
 * CAN 接收与固件升级由 can_fw_upgrade 库自包含管理;
 * 本模块仅注册业务帧 callback 并提供业务帧发送。
 */

#include <string.h>
#ifdef CONFIG_GW_NETWORKING
#include <arpa/inet.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <can_fw_upgrade.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_can, LOG_LEVEL_INF);

const struct device *can_dev;	/* 由 gw_can_init() 从 can_fw_set_app_handler() 获取 */

/* ================================================================
 * CAN 发送 (业务帧响应)
 * ================================================================ */
static void can_tx_callback(const struct device *dev, int error, void *user_data)
{
	if (error) {
		LOG_ERR("CAN tx error: %d", error);
	}
}

int gw_can_send(uint16_t id, const uint8_t *data, uint8_t len)
{
	if (!can_dev) {
		return -ENODEV;
	}

	struct can_frame frame = {
		.id = id,
		.dlc = can_bytes_to_dlc(len),
	};

	if (len > 0 && len <= 8) {
		memcpy(frame.data, data, len);
	}

	return can_send(can_dev, &frame, K_MSEC(100), can_tx_callback, NULL);
}

#ifdef CONFIG_GW_NETWORKING
/* ================================================================
 * 网络配置命令处理 (0x106)
 * ================================================================ */

/* 从 CAN 帧 4 字节 (src[0..3]) 设置 IPv4 字符串字段并持久化 */
static void net_set_ipv4(char *dst, size_t dstlen, const uint8_t *src, const char *label)
{
	struct in_addr a = { .s4_addr = {src[0], src[1], src[2], src[3]} };

	inet_ntop(AF_INET, &a, dst, dstlen);
	LOG_INF("CAN set %s: %s", label, dst);
	persist_save_network_config();
}

/* 回复一帧 NET_CONFIG_RESP: [cmd 1B][ipv4 4B][reserved 3B] */
static void net_send_ipv4_resp(uint8_t cmd, const char *src_str)
{
	uint8_t buf[8] = {0};
	struct in_addr a;

	buf[0] = cmd;
	if (inet_pton(AF_INET, src_str, &a) == 1) {
		memcpy(&buf[1], a.s4_addr, 4);
	}
	gw_can_send(NET_CONFIG_RESP, buf, 8);
}

static void handle_net_config(struct can_frame *frame)
{
	uint8_t cmd = frame->data[0];

	switch (cmd) {
	case NET_CMD_SET_IP:
		net_set_ipv4(gw_params.ip_addr, sizeof(gw_params.ip_addr), &frame->data[1], "IP");
		break;
	case NET_CMD_SET_MASK:
		net_set_ipv4(gw_params.netmask, sizeof(gw_params.netmask), &frame->data[1], "mask");
		break;
	case NET_CMD_SET_GW:
		net_set_ipv4(gw_params.gateway, sizeof(gw_params.gateway), &frame->data[1], "gw");
		break;
	case NET_CMD_SET_PORT:
		gw_params.udp_port = sys_get_be16(&frame->data[1]);
		LOG_INF("CAN set port: %d", gw_params.udp_port);
		persist_save_network_config();
		break;
	case NET_CMD_SET_MODE:
		if (frame->data[1] == GW_MODE_CAN || frame->data[1] == GW_MODE_UDP) {
			gw_params.connect_type = frame->data[1];
			LOG_INF("CAN set mode: %s", gw_params.connect_type == GW_MODE_CAN ? "CAN" : "UDP");
			persist_save_network_config();
		}
		break;
	case NET_CMD_GET_CONFIG:
		break;
	default:
		LOG_WRN("CAN net config: unknown cmd 0x%02x", cmd);
		return;
	}

	/* 回复当前配置三帧: [ip] [mask+port] [gw] */
	net_send_ipv4_resp(cmd, gw_params.ip_addr);

	uint8_t buf[8] = {0};
	struct in_addr m;

	if (inet_pton(AF_INET, gw_params.netmask, &m) == 1) {
		memcpy(&buf[1], m.s4_addr, 4);
	}
	sys_put_be16(gw_params.udp_port, &buf[5]);
	gw_can_send(NET_CONFIG_RESP, buf, 8);

	net_send_ipv4_resp(0, gw_params.gateway);
}
#endif /* CONFIG_GW_NETWORKING */

/* ================================================================
 * 业务帧 handler (注入 can_fw_upgrade 库)
 * 库 RX 线程收到非固件升级帧时调用。
 * ================================================================ */
static bool gw_can_app_rx(struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (frame->id) {
	case OVERBREAK_LASER:
	case COORD_XY:
	case COORD_Z:
		/* 上位机通过 CAN 发送的扫描仪数据, 转发到 nRF24 给 mod_handler */
		gw_rf24_send(frame->id, frame->data, can_dlc_to_bytes(frame->dlc));
		LOG_DBG("CAN->nRF24: id=0x%03x dlc=%d", frame->id, frame->dlc);
		return true;

	case RF24_CONFIG_CMD: {
		/* 本地 nRF24 配置命令 */
		uint8_t cmd = frame->data[0];

		if (cmd == RF24_CMD_SET_CHANNEL && frame->dlc >= 2) {
			gw_params.rf24_channel = frame->data[1];
			persist_save_rf24_config();
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
		} else if (cmd == RF24_CMD_SET_ADDR && frame->dlc >= 7) {
			memcpy(gw_params.rf24_addr, &frame->data[1], RF24_ADDR_LEN);
			persist_save_rf24_config();
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
		}
		/* 回复当前配置 */
		uint8_t buf[8] = {0};

		buf[0] = cmd;
		buf[1] = gw_params.rf24_channel;
		memcpy(&buf[2], gw_params.rf24_addr, RF24_ADDR_LEN);
		gw_can_send(RF24_CONFIG_RESP, buf, 8);
		return true;
	}

#ifdef CONFIG_GW_NETWORKING
	case NET_CONFIG_CMD:
		/* 网络配置命令 */
		handle_net_config(frame);
		return true;
#endif

	default:
		return false;
	}
}

static int gw_can_init(void)
{
	can_dev = can_fw_set_app_handler(gw_can_app_rx, NULL);
	LOG_INF("CAN forward ready");
	return 0;
}
SYS_INIT(gw_can_init, APPLICATION, 10);
