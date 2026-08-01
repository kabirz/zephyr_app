/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 透传模块 (测试版) — 上位机与设备之间的双向 UDP 转发
 * + 网络配置命令处理 (固件升级由 udp_fw_upgrade 库自管)
 *
 * 平移自 gateway/src/udp.c, 去掉 nRF24 无线部分, 数据端口收到的帧
 * 按 gut_params.echo 开关决定是否原样回发 (供 shell 测试).
 *
 * 双端口架构:
 *   - 数据端口 (默认 9090, 可通过 UDP_CMD_SET_CONFIG 配置, 持久化):
 *       上位机数据收发 + (echo 开启时) 原样回显
 *       转发策略: 同子网 → 单播到学习到的源端口; 跨子网/未学习 → 广播到 本地+1
 *   - 配置端口 (固定 9200, 由 udp_fw_upgrade 库自管):
 *       库内部处理固件升级命令 (FW_START/DATA/END),
 *       其余配置命令 (IP/掩码/网关/端口/重启) 通过回调分发到此模块.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/byteorder.h>
#include <gateway_udp_test.h>
#include <udp_fw_upgrade.h>

LOG_MODULE_REGISTER(gut_udp, LOG_LEVEL_INF);

/* 网络就绪事件位 (由 main 的 net_init 成功后 set). 数据端口线程在 bind 前等待此位,
 * 因为 PHY/MAC 接口注册晚于 K_THREAD_DEFINE 线程启动, 早 bind 会因无接口
 * 返回 EADDRNOTAVAIL. 配置端口由 udp_fw_upgrade 库通过 net_mgmt IF_UP 自管. */
#define NET_READY_BIT 0x1

enum udp_cmd {
	/* 业务命令从 0x10 起 (0x01-0x05 由 udp_fw_upgrade 库内部处理:
	 *   1=FW_START 2=FW_DATA 3=FW_END 4=GET_VERSION 5=REBOOT) */
	UDP_CMD_SET_CONFIG = 0x10,  /* 一次性设置 IP/掩码/网关/端口/RF24信道/地址 */
	UDP_CMD_GET_CONFIG = 0x11,
};

/* 数据端口 socket + 远端地址 (数据转发目标).
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

	/* 本机 IP 从 gut_params 解析 (net_init 设置的静态 IP) */
	if (net_addr_pton(AF_INET, gut_params.ip_addr, &local_ip) < 0) {
		return true;
	}
	struct net_in_addr nm = net_if_ipv4_get_netmask_by_addr(
		iface, (const struct net_in_addr *)&local_ip);

	mask = *(struct in_addr *)&nm;
	return (sender_ip.s_addr & mask.s_addr) == (local_ip.s_addr & mask.s_addr);
}

/* 数据端口发送: → 上位机. 同子网单播到 data_remote_addr, 跨子网广播.
 * (与 udp_fw_reply 回复策略一致) */
void gut_udp_send(const uint8_t *data, size_t len)
{
	if (data_sock < 0 || len == 0) {
		return;
	}

	struct sockaddr_in dst;

	if (is_same_subnet(data_remote_addr.sin_addr)) {
		dst = data_remote_addr;
	} else {
		dst.sin_family = AF_INET;
		dst.sin_port = htons(gut_params.data_port + 1);
		dst.sin_addr.s_addr = INADDR_BROADCAST;
	}

	sendto(data_sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ================================================================
 * 配置命令处理 (应用回调, 由 udp_fw_upgrade 库 RX 线程调用)
 * 处理业务命令: UDP_CMD_SET_CONFIG (0x10) / UDP_CMD_GET_CONFIG (0x11);
 * 固件升级及版本/重启命令 (0x01-0x05) 由库内部处理, 不会到达此处.
 * 回复通过 udp_fw_reply 发送 (库自管 socket + 回复路由).
 * ================================================================ */
static bool app_cmd_handler(uint8_t cmd, const uint8_t *cmd_data, size_t cmd_len,
			    void *user_data)
{
	switch (cmd) {
	case UDP_CMD_SET_CONFIG: {
		/* 一次性设置 IP/掩码/网关/端口/RF24 信道/RF24 地址.
		 * 帧格式: [ip 4B][mask 4B][gw 4B][port 2B BE][rf24_ch 1B][rf24_addr 5B] = 20B
		 * 全部解析成功后统一持久化 (网络+RF24 各一次).
		 * rf24_ch 非法 (>125) 时保持原值不更新, 但不拒绝整包.
		 * 回复: 设置后的 20B 配置 (二进制, 与请求同序), 供上位机确认 */
		if (cmd_len >= 20) {
			struct in_addr addr;

			memcpy(&addr.s_addr, cmd_data, 4);
			inet_ntop(AF_INET, &addr, gut_params.ip_addr, sizeof(gut_params.ip_addr));
			memcpy(&addr.s_addr, cmd_data + 4, 4);
			inet_ntop(AF_INET, &addr, gut_params.netmask, sizeof(gut_params.netmask));
			memcpy(&addr.s_addr, cmd_data + 8, 4);
			inet_ntop(AF_INET, &addr, gut_params.gateway, sizeof(gut_params.gateway));
			gut_params.data_port = sys_get_be16(cmd_data + 12);
			uint8_t ch = cmd_data[14];

			if (ch <= RF24_ADDR_MAX_CH) {
				gut_params.rf24_channel = ch;
			}
			memcpy(gut_params.rf24_addr, cmd_data + 15, RF24_ADDR_LEN);

			LOG_INF("UDP set config: ip=%s mask=%s gw=%s port=%d rf24 ch=%d",
				gut_params.ip_addr, gut_params.netmask,
				gut_params.gateway, gut_params.data_port, gut_params.rf24_channel);
			persist_save_network_config();
			persist_save_rf24_config();

			/* 回复设置后的配置 (二进制 20B, 与请求同序) */
			uint8_t resp[20];
			int off = 0;

			if (inet_pton(AF_INET, gut_params.ip_addr, &addr) == 1) {
				memcpy(resp + off, &addr.s_addr, 4);
			}
			off += 4;
			if (inet_pton(AF_INET, gut_params.netmask, &addr) == 1) {
				memcpy(resp + off, &addr.s_addr, 4);
			}
			off += 4;
			if (inet_pton(AF_INET, gut_params.gateway, &addr) == 1) {
				memcpy(resp + off, &addr.s_addr, 4);
			}
			off += 4;
			sys_put_be16(gut_params.data_port, resp + off);
			off += 2;
			resp[off++] = gut_params.rf24_channel;
			memcpy(resp + off, gut_params.rf24_addr, RF24_ADDR_LEN);
			off += RF24_ADDR_LEN;

			udp_fw_reply(cmd, resp, off);
		}
		return true;
	}

	case UDP_CMD_GET_CONFIG: {
		/* 22B 格式 (与 gateway 一致, 上位机按响应长度识别):
		 * [rf24_ch 1B][rf24_addr 5B][data_port 2B][config_port 2B]
		 * [ip 4B][mask 4B][gw 4B]
		 * remote_port = data_port + 1 由上位机自行计算 (固件不传) */
		uint8_t buf[22] = {0};
		int offset = 0;

		buf[offset++] = gut_params.rf24_channel;
		memcpy(buf + offset, gut_params.rf24_addr, RF24_ADDR_LEN);
		offset += RF24_ADDR_LEN;
		sys_put_be16(gut_params.data_port, buf + offset);
		offset += 2;
		sys_put_be16(GUT_CONFIG_PORT, buf + offset);
		offset += 2;

		struct in_addr addr;

		if (inet_pton(AF_INET, gut_params.ip_addr, &addr) == 1) {
			memcpy(buf + offset, &addr.s_addr, 4);
		}
		offset += 4;
		if (inet_pton(AF_INET, gut_params.netmask, &addr) == 1) {
			memcpy(buf + offset, &addr.s_addr, 4);
		}
		offset += 4;
		if (inet_pton(AF_INET, gut_params.gateway, &addr) == 1) {
			memcpy(buf + offset, &addr.s_addr, 4);
		}
		offset += 4;

		udp_fw_reply(cmd, buf, offset);
		LOG_INF("[DIAG] GET_CONFIG resp len=%d", offset);
		return true;
	}

	default:
		return false;
	}
}

/* ================================================================
 * 数据端口接收线程 (绑定 gut_params.data_port, 默认 9090)
 * 收到上位机数据帧 → 按 echo 开关原样回发 (供测试); 同时学习 data_remote_addr
 * ================================================================ */
static void udp_data_rx_thread(void)
{
	/* 等待网络接口就绪 (main 中 net_init 成功) 再创建 socket, 否则 bind
	 * 会因无可用接口返回 EADDRNOTAVAIL. */
	k_event_wait(&gut_params.event, NET_READY_BIT, false, K_FOREVER);

	data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (data_sock < 0) {
		LOG_ERR("data socket create failed: %d", errno);
		return;
	}

	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(gut_params.data_port),
	};

	if (bind(data_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		LOG_ERR("data socket bind failed: %d", errno);
		close(data_sock);
		data_sock = -1;
		return;
	}

	LOG_INF("data port %d listening", gut_params.data_port);

	/* 默认广播目标: 未学习到同子网发送方前, 数据以广播发出 */
	data_remote_addr.sin_family = AF_INET;
	data_remote_addr.sin_port = htons(gut_params.data_port);
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

		if (is_same_subnet(src_addr.sin_addr)) {
			data_remote_addr = src_addr;
		}

		if (received >= 2) {
			uint16_t can_id = sys_get_be16(buf);

			LOG_DBG("UDP RX: id=0x%03x len=%zd", can_id, received);

			/* echo 模式: 原样回发收到的帧 (含帧 ID) */
			if (gut_params.echo) {
				gut_udp_send(buf, received);
			}
		}
	}
}

K_THREAD_DEFINE(thread_udp_data_rx, CONFIG_GUT_DATA_RX_STACK, udp_data_rx_thread, NULL, NULL,
		NULL, CONFIG_GUT_DATA_RX_PRIORITY, 0, 0);

/* ================================================================
 * 初始化: 注册配置命令回调 (固件升级库通过 net_mgmt IF_UP 自管配置端口 socket)
 * ================================================================ */
static int gut_udp_init(void)
{
	udp_fw_set_app_handler(app_cmd_handler, NULL);
	return 0;
}

SYS_INIT(gut_udp_init, APPLICATION, CONFIG_GUT_UDP_INIT_PRIORITY);
