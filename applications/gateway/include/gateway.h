/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gateway application common definitions
 */

#ifndef __GATEWAY_H__
#define __GATEWAY_H__

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

/* ================================================================
 * RF24 地址/信道配置
 * ================================================================ */
#define RF24_ADDR_LEN    5
#define RF24_ADDR_MAX_CH 125
#define RF24_DEFAULT_CH  76

/* ================================================================
 * 帧 ID (与 mod_handler 保持一致; nRF24/UDP 帧协议的逻辑标识符,
 * 复用历史 CAN 11-bit 编号, 已与 CAN 总线无关)
 * ================================================================ */
enum can_ids {
	RF24_CONFIG_CMD = 0x104,
	RF24_CONFIG_RESP = 0x105,
	NET_CONFIG_CMD = 0x106,   /* 平台→网关: 网络配置命令 */
	NET_CONFIG_RESP = 0x107,  /* 网关→平台: 网络配置响应 */
	COBID_HEATBEAT = 0x763,
	TEST_FRAME = 0x777,
	HANDLER_STATE = 0x1E3,
	OVERBREAK_LASER = 0x263,
	COORD_XY = 0x363,
	COORD_Z = 0x463,
};

/* RF24_CONFIG_CMD 命令类型 */
enum rf24_config_cmd {
	RF24_CMD_SET_CHANNEL = 0x01,
	RF24_CMD_GET_CONFIG = 0x02,
	RF24_CMD_SET_ADDR = 0x03,    /* 设置地址: [0x03][addr 5B][reserved 2B] */
};

/* NET_CONFIG_CMD 命令类型 */
enum net_config_cmd {
	NET_CMD_SET_IP = 0x01,       /* [0x01][ip 4B] */
	NET_CMD_SET_MASK = 0x02,     /* [0x02][mask 4B] */
	NET_CMD_SET_GW = 0x03,       /* [0x03][gw 4B] */
	NET_CMD_SET_PORT = 0x04,     /* [0x04][port 2B BE] */
	NET_CMD_GET_CONFIG = 0x05,   /* [0x05] 查询全部配置 */
};

/* ================================================================
 * 网络默认配置
 * ================================================================ */
#define GATEWAY_DEFAULT_IP       "192.168.1.100"
#define GATEWAY_DEFAULT_MASK     "255.255.255.0"
#define GATEWAY_DEFAULT_GW       "192.168.1.1"
#define GATEWAY_DEFAULT_UDP_PORT 9000

/* ================================================================
 * 全局状态
 * ================================================================ */
typedef struct {
	/* RF24 配置 */
	uint8_t rf24_channel;
	uint8_t rf24_addr[RF24_ADDR_LEN];

	/* 网络配置 */
	char ip_addr[16];
	char netmask[16];
	char gateway[16];
	uint16_t udp_port;

	/* 运行状态 */
	volatile bool running;
	struct k_event event;
} gateway_params_t;

extern gateway_params_t gw_params;

/* ================================================================
 * 接口声明
 * ================================================================ */

/* rf24.c */
void gw_rf24_init(void);
void gw_rf24_set_config(uint8_t channel, const uint8_t *addr);
bool gw_rf24_send(uint16_t can_id, const uint8_t *data, size_t len);

/**
 * @brief 测试帧 (TEST_FRAME 0x777) 接收回调 (rf24_shell.c 实现)
 */
void rf24_test_handle_rx(const uint8_t *data, uint8_t len);

/* udp_forward.c */
void gw_udp_send(const uint8_t *data, size_t len);

/* config.c */
void gw_config_save(void);
void gw_config_load(void);

/* persist.c */
void persist_save_rf24_config(void);
void persist_save_network_config(void);

#endif /* __GATEWAY_H__ */
