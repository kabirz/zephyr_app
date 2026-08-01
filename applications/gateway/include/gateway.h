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
	COBID_HEATBEAT = 0x763,
	TEST_FRAME = 0x777,
	HANDLER_STATE = 0x1E3,
	OVERBREAK_LASER = 0x263,
	COORD_XY = 0x363,
	COORD_Z = 0x463,
};

/* UDP 配置命令 (走配置端口 9200, 由 udp_fw_upgrade 库 RX 线程分发到 app_cmd_handler).
 * 0x01-0x05 由库内部处理 (FW_START/DATA/END/GET_VERSION/REBOOT), 不会到达此处.
 * 掩码固定 255.255.255.0, 网关 = IP 末段改 1 (a.b.c.1), 均不在帧中传输. */
enum udp_cmd {
	UDP_CMD_SET_NET  = 0x12,   /* [ip 4B][port 2B BE] = 6B → 回显同序 6B */
	UDP_CMD_GET_NET  = 0x13,   /* (空) → [ip 4B][port 2B BE] = 6B */
	UDP_CMD_SET_RF24 = 0x14,   /* [ch 1B][addr 5B] = 6B → 回显同序 6B */
	UDP_CMD_GET_RF24 = 0x15,   /* (空) → [ch 1B][addr 5B] = 6B */
};

/* ================================================================
 * 网络默认配置
 * 掩码固定 255.255.255.0; 网关 = IP 末段改 1 (运行时派生, 不存储)
 * ================================================================ */
#define GATEWAY_DEFAULT_IP       "192.168.1.100"
#define GATEWAY_DATA_PORT_DEFAULT 9090  /* 数据端口 (可配, UDP_CMD_SET_NET) */
#define GATEWAY_CONFIG_PORT      9200  /* 配置端口 (固定, 不受 SET_NET 影响) */

/* ================================================================
 * 全局状态
 * ================================================================ */
typedef struct {
	/* RF24 配置 */
	uint8_t rf24_channel;
	uint8_t rf24_addr[RF24_ADDR_LEN];

	/* 网络配置 (掩码固定 /24, 网关由 IP 派生, 均不存储) */
	char ip_addr[16];
	uint16_t data_port;   /* 数据端口 (可配, 默认 GATEWAY_DATA_PORT_DEFAULT) */

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

/* udp.c */
void gw_udp_send(const uint8_t *data, size_t len);

/* config.c */
void gw_config_save(void);
void gw_config_load(void);

/* persist.c */
void persist_save_rf24_config(void);
void persist_save_network_config(void);

#endif /* __GATEWAY_H__ */
