/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 透传模块 - nRF24 与上位机之间的双向 UDP 转发
 * + 网络配置 + 固件升级 (通过 UDP)
 * (移植自 gateway: flash 写入层 stub, 去 settings 持久化)
 *
 * 双端口架构 (数据/配置端口均支持广播收发, 均按子网判断单播/广播):
 *   - 数据端口 (默认 9090, 可通过 UDP_CMD_SET_PORT 配置):
 *       nRF24 → 上位机数据转发 (gw_udp_send) + 上位机 → nRF24 扫描仪数据透传
 *       转发策略: 目标与本机同子网 → 单播; 跨子网/未学习 → 广播
 *   - 配置端口 (固定 9200, 不可改):
 *       所有配置命令 (IP/掩码/网关/端口/RF24/重启/固件升级)
 *       支持广播接收 (上位机不知道设备 IP 时可广播配置)
 *       回复策略: 发送方与本机同子网 → 单播; 跨子网 → 广播
 *
 * 配置命令帧格式: [cmd 1B][data...] (无魔数头, 配置端口只收命令)
 * 数据帧格式: [帧 ID 2B BE][payload]  (帧 ID 见 enum can_ids, 复用历史编号)
 *
 * gw_sim 差异: 配置只存内存 (无 persist); 固件升级 flash 写入改 stub (计数+日志).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/app_version.h>
/* native_sim NATIVE_LIBC: socket() 走主机 glibc, 须用 glibc 的 socket 头与类型
 * (Zephyr net_ip.h 的 sockaddr_in 为 8B, 与 glibc 16B 不一致 → bind EINVAL;
 *  net_compat.h AF_INET=1 与 Linux AF_INET=2 冲突). 直接 include 主机 glibc 头. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_udp, LOG_LEVEL_INF);


enum udp_cmd {
	UDP_CMD_SET_IP = 0x01,
	UDP_CMD_SET_MASK = 0x02,
	UDP_CMD_SET_GW = 0x03,
	UDP_CMD_SET_PORT = 0x04,
	UDP_CMD_GET_CONFIG = 0x05,
	UDP_CMD_GET_VERSION = 0x06,
	UDP_CMD_SET_RF24_CH = 0x07,
	UDP_CMD_SET_RF24_ADDR = 0x08,
	UDP_CMD_REBOOT = 0x09,
	UDP_CMD_FW_START = 0x10,
	UDP_CMD_FW_DATA = 0x11,
	UDP_CMD_FW_END = 0x12,
};

/* 数据端口 socket + 远端地址 (nRF24 数据转发目标).
 * data_remote_addr 为最近一次同子网数据发送方地址; 跨子网或未学习时为广播 */
static int data_sock = -1;
static struct sockaddr_in data_remote_addr;

/* 配置端口 socket + 远端地址 (配置命令回复目标).
 * config_remote_addr 为最近一次配置命令发送方地址 */
static int config_sock = -1;
static struct sockaddr_in config_remote_addr;

/* ================================================================
 * 固件升级状态 (stub: 无 flash 写入, 仅计数验证协议分包)
 * ================================================================ */
static bool fw_started = false;
static size_t fw_total_bytes;

static void fw_reset(void)
{
	if (fw_started) {
		fw_started = false;
		LOG_INF("FW upgrade complete");
	}
}

/* ================================================================
 * 子网判断 + UDP 发送
 * ================================================================ */

/* 判断发送方 IP 是否与本机同子网 (按 netmask 计算).
 * 数据/配置端口均用此函数决定单播目标, 无法判断时按同子网 (单播) */
static bool is_same_subnet(struct in_addr sender_ip)
{
	/* nsos 主机网络模式: 发送方都在主机网络, 直接单播回复 (避免 gw_params 静态 IP
	 * 与主机实际 IP 不一致导致误判广播). */
	ARG_UNUSED(sender_ip);
	return true;
}

/* 数据端口发送: nRF24 数据 → 上位机. 同子网单播到 data_remote_addr, 跨子网广播.
 * (与 config_send_resp 回复策略一致) */
void gw_udp_send(const uint8_t *data, size_t len)
{
	if (data_sock < 0 || len == 0) {
		return;
	}

	struct sockaddr_in dst;

	if (is_same_subnet(data_remote_addr.sin_addr)) {
		dst = data_remote_addr;
	} else {
		dst.sin_family = AF_INET;
		dst.sin_port = htons(gw_params.data_port);
		dst.sin_addr.s_addr = INADDR_BROADCAST;
	}

	sendto(data_sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* 配置端口回复: 通过配置 socket 回复. 同子网单播给发送方, 跨子网广播.
 * 回复格式: [cmd 1B][data...] (无魔数头) */
static void config_send_resp(uint8_t cmd, const uint8_t *data, uint8_t len)
{
	if (config_sock < 0) {
		return;
	}

	uint8_t buf[64] = {0};
	size_t send_len = 1; /* cmd */

	buf[0] = cmd;
	if (len > 0 && len <= sizeof(buf) - 1) {
		memcpy(buf + 1, data, len);
		send_len += len;
	}

	/* 同子网单播回复; 跨子网广播回复 (让跨网段的上位机也能收到) */
	struct sockaddr_in dst;

	if (is_same_subnet(config_remote_addr.sin_addr)) {
		dst = config_remote_addr;
	} else {
		dst.sin_family = AF_INET;
		dst.sin_port = htons(GATEWAY_CONFIG_PORT);
		dst.sin_addr.s_addr = INADDR_BROADCAST;
	}

	sendto(config_sock, buf, send_len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ================================================================
 * UDP 命令处理
 * ================================================================ */
static void udp_cmd_handler(const uint8_t *data, size_t len)
{
	if (len < 1) {
		return;
	}

	uint8_t cmd = data[0];
	const uint8_t *cmd_data = data + 1;
	size_t cmd_len = len - 1;

	switch (cmd) {
	case UDP_CMD_SET_IP:
		if (cmd_len >= 4) {
			struct in_addr addr;

			memcpy(&addr.s_addr, cmd_data, 4);
			inet_ntop(AF_INET, &addr, gw_params.ip_addr, sizeof(gw_params.ip_addr));
			LOG_INF("UDP set IP: %s", gw_params.ip_addr);
			config_send_resp(cmd, (uint8_t *)gw_params.ip_addr, strlen(gw_params.ip_addr));
		}
		break;

	case UDP_CMD_SET_MASK:
		if (cmd_len >= 4) {
			struct in_addr mask;

			memcpy(&mask.s_addr, cmd_data, 4);
			inet_ntop(AF_INET, &mask, gw_params.netmask, sizeof(gw_params.netmask));
			LOG_INF("UDP set mask: %s", gw_params.netmask);
			config_send_resp(cmd, (uint8_t *)gw_params.netmask, strlen(gw_params.netmask));
		}
		break;

	case UDP_CMD_SET_GW:
		if (cmd_len >= 4) {
			struct in_addr gw;

			memcpy(&gw.s_addr, cmd_data, 4);
			inet_ntop(AF_INET, &gw, gw_params.gateway, sizeof(gw_params.gateway));
			LOG_INF("UDP set gw: %s", gw_params.gateway);
			config_send_resp(cmd, (uint8_t *)gw_params.gateway, strlen(gw_params.gateway));
		}
		break;

	case UDP_CMD_SET_PORT:
		if (cmd_len >= 2) {
			gw_params.data_port = sys_get_be16(cmd_data);
			LOG_INF("UDP set data port: %d", gw_params.data_port);
			config_send_resp(cmd, cmd_data, 2);
		}
		break;

	case UDP_CMD_GET_CONFIG: {
		uint8_t buf[32] = {0};
		int offset = 0;

		buf[offset++] = gw_params.rf24_channel;
		memcpy(buf + offset, gw_params.rf24_addr, RF24_ADDR_LEN);
		offset += RF24_ADDR_LEN;
		sys_put_be16(gw_params.data_port, buf + offset);
		offset += 2;
		sys_put_be16(GATEWAY_CONFIG_PORT, buf + offset);
		offset += 2;
		config_send_resp(cmd, buf, offset);
		break;
	}

	case UDP_CMD_GET_VERSION:
		/* 响应 APP_VERSION_STRING (含 EXTRAVERSION, 如 "0.1.0-dev").
		 * 变长字符串, 不含末尾 '\0' */
		config_send_resp(cmd, (const uint8_t *)APP_VERSION_STRING,
				 strlen(APP_VERSION_STRING));
		break;

	case UDP_CMD_SET_RF24_CH:
		if (cmd_len >= 1 && cmd_data[0] <= RF24_ADDR_MAX_CH) {
			gw_params.rf24_channel = cmd_data[0];
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
			LOG_INF("UDP set rf24 ch: %d", gw_params.rf24_channel);
		}
		config_send_resp(cmd, &gw_params.rf24_channel, 1);
		break;

	case UDP_CMD_SET_RF24_ADDR:
		if (cmd_len >= RF24_ADDR_LEN) {
			memcpy(gw_params.rf24_addr, cmd_data, RF24_ADDR_LEN);
			gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);
			LOG_INF("UDP set rf24 addr");
		}
		config_send_resp(cmd, gw_params.rf24_addr, RF24_ADDR_LEN);
		break;

	case UDP_CMD_REBOOT:
		LOG_INF("UDP reboot requested");
		config_send_resp(cmd, NULL, 0);
		k_msleep(100);
		sys_reboot(SYS_REBOOT_COLD);
		break;

	case UDP_CMD_FW_START:
		if (!fw_started) {
			fw_started = true;
			fw_total_bytes = 0;
			LOG_INF("FW upgrade started (stub, no flash write)");
		}
		config_send_resp(cmd, (uint8_t *)"ok", 2);
		break;

	case UDP_CMD_FW_DATA:
		if (fw_started && cmd_len > 0) {
			fw_total_bytes += cmd_len;
			LOG_DBG("FW chunk: +%zu (total %zu)", cmd_len, fw_total_bytes);
			config_send_resp(cmd, (uint8_t *)"ok", 2);
		}
		break;

	case UDP_CMD_FW_END:
		if (fw_started) {
			LOG_INF("FW upgrade end (stub), total %zu bytes", fw_total_bytes);
			fw_reset();
			config_send_resp(cmd, (uint8_t *)"ok", 2);
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
 * 数据端口接收线程 (绑定 gw_params.data_port, 默认 9090)
 * 收到上位机扫描仪数据帧 → 透传到 nRF24; 同时学习 data_remote_addr
 * (nRF24 数据转发目标, 见 gw_udp_send)
 * ================================================================ */
static void udp_data_rx_thread(void)
{
	data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	LOG_INF("data socket(AF=%d TYPE=%d PROTO=%d) => fd=%d errno=%d",
		AF_INET, SOCK_DGRAM, IPPROTO_UDP, data_sock, errno);
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

	uint8_t buf[512];

	while (1) {
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);

		ssize_t received = recvfrom(data_sock, buf, sizeof(buf), 0,
					    (struct sockaddr *)&src_addr, &addr_len);
		if (received <= 0) {
			continue;
		}

		/* 仅学习同子网发送方地址 (供 nRF24→UDP 转发的单播目标);
		 * 跨子网的包不更新目标, gw_udp_send 仍按广播发出,
		 * 避免跨网段噪声劫持数据流 */
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
 * 配置端口接收线程 (绑定 0.0.0.0:9200, INADDR_ANY 以收广播配置命令)
 * 收到配置命令 → udp_cmd_handler; 学习 config_remote_addr (供回复)
 * ================================================================ */
static void udp_config_rx_thread(void)
{
	config_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	LOG_INF("config socket(AF=%d TYPE=%d PROTO=%d) => fd=%d errno=%d",
		AF_INET, SOCK_DGRAM, IPPROTO_UDP, config_sock, errno);
	if (config_sock < 0) {
		LOG_ERR("config socket create failed: %d", errno);
		return;
	}

	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(GATEWAY_CONFIG_PORT),
	};

	if (bind(config_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		LOG_ERR("config socket bind failed: %d", errno);
		close(config_sock);
		config_sock = -1;
		return;
	}

	LOG_INF("config port %d listening", GATEWAY_CONFIG_PORT);

	uint8_t buf[512];

	while (1) {
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);

		ssize_t received = recvfrom(config_sock, buf, sizeof(buf), 0,
					    (struct sockaddr *)&src_addr, &addr_len);
		if (received <= 0) {
			continue;
		}

		/* 记录配置端口发送方地址 (config_send_resp 按子网判断单播/广播回复) */
		config_remote_addr = src_addr;

		/* 配置端口只收命令帧, 直接交给 handler ([cmd 1B][data...]) */
		udp_cmd_handler(buf, received);
	}
}

K_THREAD_DEFINE(thread_udp_config_rx, CONFIG_GATEWAY_UDP_STACK, udp_config_rx_thread, NULL, NULL,
		NULL, CONFIG_GATEWAY_UDP_PRIORITY, 0, 0);
