/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF24L01P 接收模块 - 从 mod_handler 接收数据
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <nrf24l01p.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_rf24, LOG_LEVEL_INF);

#define RF24_PAYLOAD_MAX 32
#define RF24_ID_SIZE     2
#define RF24_TX_TIMEOUT  K_MSEC(100)

static const struct device *rf24_dev = DEVICE_DT_GET(DT_NODELABEL(nrf24));
static K_MUTEX_DEFINE(rf24_tx_mutex);

extern int gw_can_send(uint16_t id, const uint8_t *data, uint8_t len);

/* RX 帧 msgq */
K_MSGQ_DEFINE(rf24_rx_msgq, sizeof(struct nrf24_frame), 8, 4);

/* ================================================================
 * RF24 配置
 * ================================================================ */
void gw_rf24_set_config(uint8_t channel, const uint8_t *addr)
{
	gw_params.rf24_channel = channel;
	memcpy(gw_params.rf24_addr, addr, RF24_ADDR_LEN);

	if (!device_is_ready(rf24_dev)) {
		return;
	}

	struct nrf24_cfg cfg = {
		.channel = channel,
		.address_width = RF24_ADDR_LEN,
		.tx_addr = gw_params.rf24_addr,
	};

	nrf24_configure(rf24_dev, &cfg);
	nrf24_start_rx(rf24_dev);

	LOG_INF("RF24 config: ch=%d addr=%02x%02x%02x%02x%02x", channel, addr[0], addr[1], addr[2],
		addr[3], addr[4]);
}

/* ================================================================
 * RF24 初始化
 * ================================================================ */
void gw_rf24_init(void)
{
	if (!device_is_ready(rf24_dev)) {
		LOG_ERR("nRF24 device not ready");
		return;
	}

	/* 应用配置 */
	gw_rf24_set_config(gw_params.rf24_channel, gw_params.rf24_addr);

	/* 注册 msgq */
	nrf24_add_rx_msgq(rf24_dev, &rf24_rx_msgq);

	/* 进入 PRX 接收模式 */
	int ret = nrf24_start_rx(rf24_dev);

	if (ret != 0) {
		LOG_ERR("nRF24 start RX failed: %d", ret);
		return;
	}
	LOG_INF("nRF24 ready (PRX, ch=%d)", gw_params.rf24_channel);
}

/* ================================================================
 * RF24 发送 (通过 CAN 转发到 mod_handler)
 * ================================================================ */
bool gw_rf24_send(uint16_t can_id, const uint8_t *data, size_t len)
{
	if (!device_is_ready(rf24_dev)) {
		return false;
	}
	if (len > RF24_PAYLOAD_MAX - RF24_ID_SIZE) {
		return false;
	}

	uint8_t buf[RF24_PAYLOAD_MAX];

	sys_put_be16(can_id, buf);
	if (len > 0) {
		memcpy(buf + RF24_ID_SIZE, data, len);
	}

	struct nrf24_tx_result result;

	k_mutex_lock(&rf24_tx_mutex, K_FOREVER);
	int ret = nrf24_send(rf24_dev, buf, len + RF24_ID_SIZE, RF24_TX_TIMEOUT, &result);
	k_mutex_unlock(&rf24_tx_mutex);

	if (ret != 0) {
		LOG_WRN("nRF24 send failed (id=0x%03x ret=%d)", can_id, ret);
		return false;
	}
	return true;
}

/* ================================================================
 * RF24 接收线程
 * ================================================================ */
static void rf24_rx_thread(void)
{
	if (!device_is_ready(rf24_dev)) {
		LOG_ERR("nRF24 device not ready, RX thread exit");
		return;
	}

	struct nrf24_frame frame;

	while (1) {
		if (k_msgq_get(&rf24_rx_msgq, &frame, K_MSEC(200)) != 0) {
			continue;
		}
		if (frame.len < RF24_ID_SIZE) {
			continue;
		}

		uint16_t can_id = sys_get_be16(frame.data);
		uint8_t data_len = frame.len - RF24_ID_SIZE;
		const uint8_t *data = frame.data + RF24_ID_SIZE;

		/* 测试帧 (TEST_FRAME): 交给 rf24_shell 处理 (ping/echo/data) */
		if (can_id == TEST_FRAME) {
			rf24_test_handle_rx(data, data_len);
			continue;
		}

		/* 只转发 HANDLER_STATE 和心跳帧 */
		if (can_id != HANDLER_STATE && can_id != COBID_HEATBEAT) {
			continue;
		}

		/* 根据模式转发数据 */
#ifdef CONFIG_GW_NETWORKING
		if (gw_params.connect_type == GW_MODE_UDP) {
			/* UDP 模式: nRF24 数据通过 UDP 发送 */
			gw_udp_send(frame.data, frame.len);
			LOG_DBG("nRF24->UDP: id=0x%03x len=%d", can_id, frame.len);
		} else
#endif
		{
			/* CAN 模式: nRF24 数据通过 CAN 发送 */
			gw_can_send(can_id, data, data_len);
			LOG_DBG("nRF24->CAN: id=0x%03x dlc=%d", can_id, data_len);
		}
	}
}

K_THREAD_DEFINE(thread_rf24_rx, CONFIG_GATEWAY_RF24_RX_STACK, rf24_rx_thread, NULL, NULL, NULL,
		CONFIG_GATEWAY_RF24_RX_PRIORITY, 0, 0);
