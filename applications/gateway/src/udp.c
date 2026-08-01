/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 透传模块 - nRF24 与上位机之间的双向 UDP 转发
 * + 网络配置命令处理 (固件升级由 udp_fw_upgrade 库自管)
 *
 * 双端口架构:
 *   - 数据端口 (默认 9090, 可通过 UDP_CMD_SET_PORT 配置, 持久化):
 *       nRF24 → 上位机数据转发 (gw_udp_send) + 上位机 → nRF24 扫描仪数据透传
 *       转发策略: 目标与本机同子网 → 单播; 跨子网/未学习 → 广播
 *   - 配置端口 (固定 9200, 由 udp_fw_upgrade 库自管):
 *       库内部处理固件升级命令 (FW_START/DATA/END),
 *       其余配置命令 (IP/掩码/网关/端口/RF24/重启) 通过回调分发到此模块.
 *
 * 配置命令帧格式: [cmd 1B][data...] (无魔数头, 配置端口只收命令)
 * 数据帧格式: [帧 ID 2B BE][payload]  (帧 ID 见 enum can_ids, 复用历史编号)
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/byteorder.h>
#include <gateway.h>
#include <udp_fw_upgrade.h>

LOG_MODULE_REGISTER(gw_udp, LOG_LEVEL_INF);

enum udp_cmd {
	/* 业务命令从 0x10 起 (0x01-0x05 由 udp_fw_upgrade 库内部处理:
	 *   1=FW_START 2=FW_DATA 3=FW_END 4=GET_VERSION 5=REBOOT) */
	UDP_CMD_SET_IP = 0x10,
	UDP_CMD_SET_MASK = 0x11,
	UDP_CMD_SET_GW = 0x12,
	UDP_CMD_SET_PORT = 0x13,
	UDP_CMD_GET_CONFIG = 0x14,
	UDP_CMD_SET_RF24_CH = 0x15,
	UDP_CMD_SET_RF24_ADDR = 0x16,
};

/* 数据端口 socket + 远端地址 (nRF24 数据转发目标).
 * data_remote_addr 为最近一次同子网数据发送方地址; 跨子网或未学习时为广播 */
static int data_sock = -1;
static struct sockaddr_in data_remote_addr;

/* ================================================================
 * 子网判断 + 数据端口发送
 * ================================================================ */

/* 判断发送方 IP 是否与本机同子网 (按 netmask 计算).
 * 数据端口用此函数决定单播目标, 无法判断时按同子网 (单播) */
static bool is_same_subnet(struct in_addr sender_ip)
{
	struct net_if *iface = net_if_get_default();
	struct in_addr local_ip, mask;

	if (!iface) {
		return true; /* 无法判断时按同子网处理 (单播) */
	}

	/* 本机 IP 从 gw_params 解析 (net_init 设置的静态 IP) */
	if (net_addr_pton(AF_INET, gw_params.ip_addr, &local_ip) < 0) {
		return true;
	}
	struct net_in_addr nm = net_if_ipv4_get_netmask_by_addr(
		iface, (const struct net_in_addr *)&local_ip);

	mask = *(struct in_addr *)&nm;
	return (sender_ip.s_addr & mask.s_addr) == (local_ip.s_addr & mask.s_addr);
}

/* 数据端口发送: nRF24 数据 → 上位机. 同子网单播到 data_remote_addr, 跨子网广播. */
void gw_udp_send(const uint8_t *data, size_t len)
{
	if (data_sock < 0 || len == 0) {
		return;
	}

	struct sockaddr_in dst;

	if (is_same_subnet(data_remote_addr.sin_addr)) {
		/* 同子网: 单播到学习到的上位机源地址 (端口=发送方源端口) */
		dst = data_remote_addr;
	} else {
		/* 跨子网/未学习: 广播. 远程端口 = 本地端口 + 1 (约定上位机监听
		 * data_port+1, 本地绑定 data_port; 单播时用源端口, 不受此约定影响) */
		dst.sin_family = AF_INET;
		dst.sin_port = htons(gw_params.data_port + 1);
		dst.sin_addr.s_addr = INADDR_BROADCAST;
	}

	sendto(data_sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ================================================================
 * 配置命令处理 (应用回调, 由 udp_fw_upgrade 库 RX 线程调用)
 * 处理 IP/掩码/网关/端口/RF24/版本/重启等业务命令;
 * 固件升级命令 (0x10/0x11/0x12) 由库内部处理, 不会到达此处.
 * 回复通过 udp_fw_reply 发送 (库自管 socket + 回复路由).
 * ================================================================ */
static bool app_cmd_handler(uint8_t cmd, const uint8_t *cmd_data, size_t cmd_len,
			    void *user_data)
{
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
			udp_fw_reply(cmd, (uint8_t *)gw_params.ip_addr, strlen(gw_params.ip_addr));
		}
		return true;

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
			udp_fw_reply(cmd, (uint8_t *)gw_params.netmask, strlen(gw_params.netmask));
		}
		return true;

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
			udp_fw_reply(cmd, (uint8_t *)gw_params.gateway, strlen(gw_params.gateway));
		}
		return true;

	case UDP_CMD_SET_PORT:
		if (cmd_len >= 2) {
			gw_params.data_port = sys_get_be16(cmd_data);
			LOG_INF("UDP set data port: %d", gw_params.data_port);
			persist_save_network_config();
			udp_fw_reply(cmd, cmd_data, 2);
		}
		return true;

	case UDP_CMD_GET_CONFIG: {
		/* 响应格式 (向后兼容, 上位机按长度识别):
		 *   [rf24_ch 1B][rf24_addr 5B][data_port 2B][remote_port 2B][config_port 2B][ip 4B][mask 4B][gw 4B] = 24B
		 * remote_port = data_port + 1 (gateway 广播目标端口规则) */
		uint8_t buf[32] = {0};
		int offset = 0;

		buf[offset++] = gw_params.rf24_channel;
		memcpy(buf + offset, gw_params.rf24_addr, RF24_ADDR_LEN);
		offset += RF24_ADDR_LEN;
		sys_put_be16(gw_params.data_port, buf + offset);
		offset += 2;
		sys_put_be16(gw_params.data_port + 1, buf + offset);
		offset += 2;
		sys_put_be16(GATEWAY_CONFIG_PORT, buf + offset);
		offset += 2;

		struct in_addr addr;

		if (inet_pton(AF_INET, gw_params.ip_addr, &addr) == 1) {
			memcpy(buf + offset, &addr.s_addr, 4);
		}
		offset += 4;
		if (inet_pton(AF_INET, gw_params.netmask, &addr) == 1) {
			memcpy(buf + offset, &addr.s_addr, 4);
		}
		offset += 4;
		if (inet_pton(AF_INET, gw_params.gateway, &addr) == 1) {
			memcpy(buf + offset, &addr.s_addr, 4);
		}
		offset += 4;

		udp_fw_reply(cmd, buf, offset);
		return true;
	}

	case UDP_CMD_SET_RF24_CH:
		if (cmd_len >= 1 && cmd_data[0] <= RF24_ADDR_MAX_CH) {
			gw_params.rf24_channel = cmd_data[0];
			persist_save_rf24_config();
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
			LOG_INF("UDP set rf24 ch: %d", gw_params.rf24_channel);
		}
		udp_fw_reply(cmd, &gw_params.rf24_channel, 1);
		return true;

	case UDP_CMD_SET_RF24_ADDR:
		if (cmd_len >= RF24_ADDR_LEN) {
			memcpy(gw_params.rf24_addr, cmd_data, RF24_ADDR_LEN);
			persist_save_rf24_config();
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
			LOG_INF("UDP set rf24 addr");
		}
		udp_fw_reply(cmd, gw_params.rf24_addr, RF24_ADDR_LEN);
		return true;

	default:
		return false;
	}
}

/* ================================================================
 * 数据端口接收线程 (绑定 gw_params.data_port, 默认 9090)
 * 收到上位机扫描仪数据帧 → 透传到 nRF24; 同时学习 data_remote_addr
 * (nRF24 数据转发目标, 见 gw_udp_send)
 * ================================================================ */
static void udp_data_rx_thread(void)
{
	data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (data_sock < 0) {
		LOG_ERR("data socket create failed: %d", errno);
		return;
	}

	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(gw_params.data_port),
	};

	if (bind(data_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		LOG_ERR("data socket bind failed: %d", errno);
		close(data_sock);
		data_sock = -1;
		return;
	}

	LOG_INF("data port %d listening", gw_params.data_port);

	/* 默认广播目标: 未学习到同子网发送方前, nRF24 数据以广播发出 */
	data_remote_addr.sin_family = AF_INET;
	data_remote_addr.sin_port = htons(gw_params.data_port);
	data_remote_addr.sin_addr.s_addr = INADDR_BROADCAST;

	static uint8_t buf[512];

	while (1) {
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);

		ssize_t received = recvfrom(data_sock, buf, sizeof(buf), 0,
					    (struct sockaddr *)&src_addr, &addr_len);
		if (received <= 0) {
			continue;
		}

		/* 仅学习同子网发送方地址 (供 nRF24→UDP 转发的单播目标) */
		if (is_same_subnet(src_addr.sin_addr)) {
			data_remote_addr = src_addr;
		}

		/* 数据端口处理扫描仪数据帧透传到 nRF24 */
		if (received >= 2) {
			uint16_t can_id = sys_get_be16(buf);

			if (can_id == OVERBREAK_LASER || can_id == COORD_XY ||
			    can_id == COORD_Z) {
				gw_rf24_send(can_id, buf + 2, received - 2);
				LOG_DBG("UDP->nRF24: id=0x%03x len=%zd", can_id,
					received - 2);
			}
		}
	}
}

K_THREAD_DEFINE(thread_udp_data_rx, CONFIG_GATEWAY_DATA_RX_STACK, udp_data_rx_thread, NULL, NULL,
		NULL, CONFIG_GATEWAY_DATA_RX_PRIORITY, 0, 0);

/* ================================================================
 * 初始化: 注册配置命令回调 (固件升级库 SYS_INIT 自管配置端口 socket)
 * ================================================================ */
static int gw_udp_init(void)
{
	udp_fw_set_app_handler(app_cmd_handler, NULL);
	return 0;
}

SYS_INIT(gw_udp_init, APPLICATION, CONFIG_GATEWAY_UDP_INIT_PRIORITY);
