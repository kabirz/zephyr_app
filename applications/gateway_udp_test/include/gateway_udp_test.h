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
 * 网络默认配置
 * ================================================================ */
#define GUT_DEFAULT_IP         "192.168.1.100"
#define GUT_DEFAULT_MASK       "255.255.255.0"
#define GUT_DEFAULT_GW         "192.168.1.1"
#define GUT_DATA_PORT_DEFAULT  9090  /* 数据端口 (可配, UDP_CMD_SET_PORT) */
#define GUT_CONFIG_PORT        9200  /* 配置端口 (固定, 不受 SET_PORT 影响) */

/* ================================================================
 * 全局状态
 * ================================================================ */
typedef struct {
	/* 网络配置 */
	char ip_addr[16];
	char netmask[16];
	char gateway[16];
	uint16_t data_port;   /* 数据端口 (可配, 默认 GUT_DATA_PORT_DEFAULT) */

	/* 运行状态 */
	volatile bool running;
	bool echo;            /* 数据端口回显开关 (shell 控制) */
	struct k_event event;
} gut_params_t;

extern gut_params_t gut_params;

/* ================================================================
 * 接口声明
 * ================================================================ */

/* udp.c */
void gut_udp_send(const uint8_t *data, size_t len);

/* persist.c */
void persist_save_network_config(void);

#endif /* __GATEWAY_UDP_TEST_H__ */
