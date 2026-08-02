/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * gateway UDP 测试应用 — 公共定义
 * 从 gateway 平移网络/UDP 功能, 去掉 nRF24 无线部分。
 */

#ifndef __GATEWAY_UDP_TEST_H__
#define __GATEWAY_UDP_TEST_H__

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <stdbool.h>
#include <stdint.h>

/* ================================================================
 * 帧 ID (数据端口数据帧格式 [帧 ID 2B BE][payload])
 * 复用 gateway 历史 CAN 11-bit 编号作逻辑标识符, 与上位机协议兼容
 * ================================================================ */
enum can_ids {
	COBID_HEATBEAT = 0x763,
	TEST_FRAME = 0x777,
	HANDLER_STATE = 0x1E3,
	OVERBREAK_LASER = 0x263,
	COORD_XY = 0x363,
	COORD_Z = 0x463,
};

/* ================================================================
 * RF24 地址/信道配置 (无 nRF24 硬件, 仅用于协议验证/持久化,
 * 字段与 gateway 保持一致以便统一上位机测试)
 * ================================================================ */
#define RF24_ADDR_LEN    5
#define RF24_ADDR_MAX_CH 125
#define RF24_DEFAULT_CH  76

/* UDP 配置命令 (走配置端口 9200, 由 udp_fw_upgrade 库 RX 线程分发到 app_cmd_handler).
 * 0x01-0x05 由库内部处理 (FW_START/DATA/END/GET_VERSION/REBOOT), 不会到达此处.
 * 静态模式下掩码固定 255.255.255.0, 网关 = IP 末段改 1 (a.b.c.1), 均不在帧中传输.
 * DHCP 模式下 IP/掩码/网关由 DHCP 服务器分配, GET_NET 回复 live interface 地址. */
enum udp_cmd {
	UDP_CMD_SET_NET  = 0x12,   /* [ip 4B][port 2B BE] = 6B → 回显同序 6B */
	UDP_CMD_GET_NET  = 0x13,   /* (空) → [ip 4B][port 2B BE] = 6B (IP 取自 live interface) */
	UDP_CMD_SET_RF24 = 0x14,   /* [ch 1B][addr 5B] = 6B → 回显同序 6B */
	UDP_CMD_GET_RF24 = 0x15,   /* (空) → [ch 1B][addr 5B] = 6B */
	UDP_CMD_SET_NET_MODE = 0x16, /* [mode 1B] (0=静态,1=DHCP) → 回显 1B (持久化, 重启生效) */
	UDP_CMD_GET_NET_MODE = 0x17, /* (空) → [mode 1B] */
};

/* ================================================================
 * 网络默认配置
 * 静态模式: 掩码固定 255.255.255.0, 网关 = IP 末段改 1 (运行时派生, 不存储)
 * DHCP 模式: IP/掩码/网关由 DHCP 服务器分配
 * ================================================================ */
#define GUT_DEFAULT_IP         "192.168.1.100"
#define GUT_DATA_PORT_DEFAULT  9090  /* 数据端口 (可配, UDP_CMD_SET_NET) */
#define GUT_CONFIG_PORT        9200  /* 配置端口 (固定, 不受 SET_NET 影响) */
#define GUT_USE_DHCP_DEFAULT   0     /* 默认静态 IP (0=静态, 1=DHCP) */

/* ================================================================
 * 全局状态
 * ================================================================ */
typedef struct {
	/* RF24 配置 (无硬件, 仅协议验证用) */
	uint8_t rf24_channel;
	uint8_t rf24_addr[RF24_ADDR_LEN];

	/* 网络配置 (静态模式下掩码固定 /24, 网关由 IP 派生, 均不存储;
	 *           DHCP 模式下 IP/掩码/网关由 DHCP 分配) */
	char ip_addr[16];     /* 静态 IP (DHCP 模式下仅作 fallback) */
	uint16_t data_port;   /* 数据端口 (可配, 默认 GUT_DATA_PORT_DEFAULT) */
	uint8_t use_dhcp;     /* 0=静态 IP, 1=DHCP (持久化, 重启生效) */

	/* 运行状态 */
	volatile bool running;
	bool echo;            /* 数据端口回显开关 (shell 控制) */
	struct k_event event;
} gut_params_t;

extern gut_params_t gut_params;

/* 网络链路状态 (main.c 的 NET_EVENT_IF_UP/DOWN 回调维护).
 * gut_udp_send 据此决定是否转发数据到上位机. */
extern volatile bool gut_net_link_up;

/* ================================================================
 * 接口声明
 * ================================================================ */

/* udp.c */
void gut_udp_send(const uint8_t *data, size_t len);

/* 取本机 live IPv4 地址 (DHCP 分配或静态配置的当前地址), 失败返回 NULL.
 * DHCP 模式下 gut_params.ip_addr 是旧静态值, 需从 live interface 读. */
struct in_addr *gut_get_live_ipv4(void);

/* persist.c */
void persist_save_network_config(void);
void persist_save_rf24_config(void);

#endif /* __GATEWAY_UDP_TEST_H__ */
