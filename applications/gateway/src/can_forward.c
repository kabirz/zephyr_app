/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 转发模块 - CAN 总线与 nRF24/UDP 之间的数据中转
 * + 网络配置 + 固件升级 (使用 can_fw_upgrade 库)
 */

#include <string.h>
#include <arpa/inet.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <can_fw_upgrade.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_can, LOG_LEVEL_INF);

const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(gw_can_msgq, 16);

/* 固件升级上下文 */
static struct can_fw_ctx fw_ctx;

static int fw_can_send(struct can_frame *frame)
{
	return can_send(can_dev, frame, K_MSEC(100), NULL, NULL);
}

/* ================================================================
 * CAN 发送
 * ================================================================ */
static void can_tx_callback(const struct device *dev, int error, void *user_data)
{
	if (error) {
		LOG_ERR("CAN tx error: %d", error);
	}
}

int gw_can_send(uint16_t id, const uint8_t *data, uint8_t len)
{
	struct can_frame frame = {
		.id = id,
		.dlc = can_bytes_to_dlc(len),
	};

	if (len > 0 && len <= 8) {
		memcpy(frame.data, data, len);
	}

	return can_send(can_dev, &frame, K_MSEC(100), can_tx_callback, NULL);
}

/* ================================================================
 * 网络配置命令处理 (0x106)
 * ================================================================ */
static void handle_net_config(struct can_frame *frame)
{
	uint8_t cmd = frame->data[0];

	switch (cmd) {
	case NET_CMD_SET_IP: {
		struct in_addr addr;

		addr.s4_addr[0] = frame->data[1];
		addr.s4_addr[1] = frame->data[2];
		addr.s4_addr[2] = frame->data[3];
		addr.s4_addr[3] = frame->data[4];
		inet_ntop(AF_INET, &addr, gw_params.ip_addr, sizeof(gw_params.ip_addr));
		LOG_INF("CAN set IP: %s", gw_params.ip_addr);
		persist_save_network_config();
		break;
	}
	case NET_CMD_SET_MASK: {
		struct in_addr mask;

		mask.s4_addr[0] = frame->data[1];
		mask.s4_addr[1] = frame->data[2];
		mask.s4_addr[2] = frame->data[3];
		mask.s4_addr[3] = frame->data[4];
		inet_ntop(AF_INET, &mask, gw_params.netmask, sizeof(gw_params.netmask));
		LOG_INF("CAN set mask: %s", gw_params.netmask);
		persist_save_network_config();
		break;
	}
	case NET_CMD_SET_GW: {
		struct in_addr gw;

		gw.s4_addr[0] = frame->data[1];
		gw.s4_addr[1] = frame->data[2];
		gw.s4_addr[2] = frame->data[3];
		gw.s4_addr[3] = frame->data[4];
		inet_ntop(AF_INET, &gw, gw_params.gateway, sizeof(gw_params.gateway));
		LOG_INF("CAN set gw: %s", gw_params.gateway);
		persist_save_network_config();
		break;
	}
	case NET_CMD_SET_PORT:
		gw_params.udp_port = sys_get_be16(&frame->data[1]);
		LOG_INF("CAN set port: %d", gw_params.udp_port);
		persist_save_network_config();
		break;
	case NET_CMD_GET_CONFIG:
		break;
	default:
		LOG_WRN("CAN net config: unknown cmd 0x%02x", cmd);
		return;
	}

	/* 回复当前配置: ip + mask + gw + port, 分三帧 */
	uint8_t buf[8] = {0};

	buf[0] = cmd;
	{
		struct in_addr a;

		if (inet_pton(AF_INET, gw_params.ip_addr, &a) == 1) {
			memcpy(&buf[1], a.s4_addr, 4);
		}
	}
	gw_can_send(NET_CONFIG_RESP, buf, 8);

	buf[0] = 0;
	{
		struct in_addr m;

		if (inet_pton(AF_INET, gw_params.netmask, &m) == 1) {
			memcpy(&buf[1], m.s4_addr, 4);
		}
	}
	sys_put_be16(gw_params.udp_port, &buf[5]);
	gw_can_send(NET_CONFIG_RESP, buf, 8);

	buf[0] = 0;
	{
		struct in_addr g;

		if (inet_pton(AF_INET, gw_params.gateway, &g) == 1) {
			memcpy(&buf[1], g.s4_addr, 4);
		}
	}
	gw_can_send(NET_CONFIG_RESP, buf, 8);
}

/* ================================================================
 * CAN 接收处理
 * ================================================================ */
static void can_rx_handler(struct can_frame *frame)
{
	/* 优先交给固件升级库处理 */
	if (can_fw_rx_handler(&fw_ctx, frame)) {
		return;
	}

	switch (frame->id) {
	case HANDLER_STATE:
	case OVERBREAK_LASER:
	case COORD_XY:
	case COORD_Z:
	case COBID_HEATBEAT:
		/* 转发到 UDP */
		gw_udp_send(frame->data, can_dlc_to_bytes(frame->dlc));
		LOG_DBG("CAN->UDP: id=0x%03x dlc=%d", frame->id, frame->dlc);
		break;

	case RF24_CONFIG_CMD: {
		/* 本地 nRF24 配置命令 */
		uint8_t cmd = frame->data[0];

		if (cmd == RF24_CMD_SET_CHANNEL && frame->dlc >= 2) {
			gw_params.rf24_channel = frame->data[1];
			persist_save_rf24_config();
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
		}
		/* 回复当前配置 */
		uint8_t buf[8] = {0};

		buf[0] = cmd;
		buf[1] = gw_params.rf24_channel;
		memcpy(&buf[2], gw_params.rf24_addr, RF24_ADDR_LEN);
		gw_can_send(RF24_CONFIG_RESP, buf, 8);
		break;
	}

	case NET_CONFIG_CMD:
		/* 网络配置命令 */
		handle_net_config(frame);
		break;

	default:
		LOG_DBG("Unknown CAN id: 0x%03x", frame->id);
		break;
	}
}

/* ================================================================
 * CAN 接收线程
 * ================================================================ */
static void can_rx_thread(void)
{
	int err;
	struct can_filter filter;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");
		return;
	}

	if ((err = can_set_bitrate(can_dev, 250000)) != 0) {
		LOG_ERR("CAN set bitrate failed: %d", err);
		return;
	}
	if ((err = can_start(can_dev)) != 0) {
		LOG_ERR("CAN start failed: %d", err);
		return;
	}

	/* 初始化固件升级 (内部注册过滤器) */
	can_fw_init(&fw_ctx, can_dev, fw_can_send);

	/* 注册其他接收过滤器 */
	filter.mask = CAN_STD_ID_MASK;

	filter.id = HANDLER_STATE;
	can_add_rx_filter_msgq(can_dev, &gw_can_msgq, &filter);

	filter.id = OVERBREAK_LASER;
	can_add_rx_filter_msgq(can_dev, &gw_can_msgq, &filter);

	filter.id = COORD_XY;
	can_add_rx_filter_msgq(can_dev, &gw_can_msgq, &filter);

	filter.id = COORD_Z;
	can_add_rx_filter_msgq(can_dev, &gw_can_msgq, &filter);

	filter.id = COBID_HEATBEAT;
	can_add_rx_filter_msgq(can_dev, &gw_can_msgq, &filter);

	filter.id = RF24_CONFIG_CMD;
	can_add_rx_filter_msgq(can_dev, &gw_can_msgq, &filter);

	filter.id = NET_CONFIG_CMD;
	can_add_rx_filter_msgq(can_dev, &gw_can_msgq, &filter);

	LOG_INF("CAN ready (250Kbps)");

	struct can_frame frame;

	while (1) {
		if (k_msgq_get(&gw_can_msgq, &frame, K_FOREVER) == 0) {
			can_rx_handler(&frame);
		}
	}
}

K_THREAD_DEFINE(thread_can_rx, CONFIG_GATEWAY_CAN_FWD_STACK, can_rx_thread, NULL, NULL, NULL,
		CONFIG_GATEWAY_CAN_FWD_PRIORITY, 0, 0);
