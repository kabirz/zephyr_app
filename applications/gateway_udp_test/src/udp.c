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
 *   - 数据端口 (默认 9090, 可通过 UDP_CMD_SET_PORT 配置, 持久化):
 *       上位机数据收发 + (echo 开启时) 原样回显
 *       转发策略: 同子网 → 单播到学习到的源端口; 跨子网/未学习 → 广播到 本地+1
 *   - 配置端口 (固定 9200, 由 udp_fw_upgrade 库自管):
 *       库内部处理固件升级命令 (FW_START/DATA/END),
 *       其余配置命令 (IP/掩码/网关/端口/重启) 通过回调分发到此模块.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/app_version.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <gateway_udp_test.h>
#include <udp_fw_upgrade.h>

LOG_MODULE_REGISTER(gut_udp, LOG_LEVEL_INF);

/* 网络就绪事件位 (由 main 的 net_init 成功后 set). 数据端口线程在 bind 前等待此位,
 * 因为 PHY/MAC 接口注册晚于 K_THREAD_DEFINE 线程启动, 早 bind 会因无接口
 * 返回 EADDRNOTAVAIL. 配置端口由 udp_fw_upgrade 库通过 net_mgmt IF_UP 自管. */
#define NET_READY_BIT 0x1

enum udp_cmd {
	UDP_CMD_SET_IP = 0x01,
	UDP_CMD_SET_MASK = 0x02,
	UDP_CMD_SET_GW = 0x03,
	UDP_CMD_SET_PORT = 0x04,
	UDP_CMD_GET_CONFIG = 0x05,
	UDP_CMD_GET_VERSION = 0x06,
	UDP_CMD_REBOOT = 0x09,
	/* 固件升级命令 0x10/0x11/0x12 由 udp_fw_upgrade 库内部处理 */
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
 * 处理 IP/掩码/网关/端口/版本/重启等业务命令;
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
			inet_ntop(AF_INET, &addr, gut_params.ip_addr, sizeof(gut_params.ip_addr));
			LOG_INF("UDP set IP: %s", gut_params.ip_addr);
			persist_save_network_config();
			udp_fw_reply(cmd, (uint8_t *)gut_params.ip_addr, strlen(gut_params.ip_addr));
		}
		return true;

	case UDP_CMD_SET_MASK:
		if (cmd_len >= 4) {
			struct in_addr mask;

			mask.s4_addr[0] = cmd_data[0];
			mask.s4_addr[1] = cmd_data[1];
			mask.s4_addr[2] = cmd_data[2];
			mask.s4_addr[3] = cmd_data[3];
			inet_ntop(AF_INET, &mask, gut_params.netmask, sizeof(gut_params.netmask));
			LOG_INF("UDP set mask: %s", gut_params.netmask);
			persist_save_network_config();
			udp_fw_reply(cmd, (uint8_t *)gut_params.netmask, strlen(gut_params.netmask));
		}
		return true;

	case UDP_CMD_SET_GW:
		if (cmd_len >= 4) {
			struct in_addr gw;

			gw.s4_addr[0] = cmd_data[0];
			gw.s4_addr[1] = cmd_data[1];
			gw.s4_addr[2] = cmd_data[2];
			gw.s4_addr[3] = cmd_data[3];
			inet_ntop(AF_INET, &gw, gut_params.gateway, sizeof(gut_params.gateway));
			LOG_INF("UDP set gw: %s", gut_params.gateway);
			persist_save_network_config();
			udp_fw_reply(cmd, (uint8_t *)gut_params.gateway, strlen(gut_params.gateway));
		}
		return true;

	case UDP_CMD_SET_PORT:
		if (cmd_len >= 2) {
			gut_params.data_port = sys_get_be16(cmd_data);
			LOG_INF("UDP set data port: %d", gut_params.data_port);
			persist_save_network_config();
			udp_fw_reply(cmd, cmd_data, 2);
		}
		return true;

	case UDP_CMD_GET_CONFIG: {
		/* 18B net_test 格式 (上位机按响应长度识别):
		 * [local_port 2B][remote_port 2B][config_port 2B][ip 4B][mask 4B][gw 4B]
		 * remote_port = local_port + 1 (与 gateway 广播目标端口规则一致) */
		uint8_t buf[24] = {0};
		int offset = 0;

		sys_put_be16(gut_params.data_port, buf + offset);
		offset += 2;
		sys_put_be16(gut_params.data_port + 1, buf + offset);
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

	case UDP_CMD_GET_VERSION:
		udp_fw_reply(cmd, (const uint8_t *)APP_VERSION_STRING,
			     strlen(APP_VERSION_STRING));
		return true;

	case UDP_CMD_REBOOT:
		LOG_INF("UDP reboot requested");
		udp_fw_reply(cmd, NULL, 0);
		k_msleep(100);
		sys_reboot(SYS_REBOOT_COLD);
		return true;

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
