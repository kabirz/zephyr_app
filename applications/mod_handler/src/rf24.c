/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 2.4G 无线通信 (nRF24L01+) — 中断驱动接收 + 请求-响应半双工
 *
 * 收发协调:
 *   nRF24 驱动内部 IRQ 线程排空 RX FIFO, 通过 rf24_rx_msgq 投递帧;
 *   本模块 RX 线程从 msgq 取帧并按 CAN ID 分发到扫描仪解析器.
 *   发送时 nrf24_send 内部切 PTX, 等待 ACK 后自动切回 PRX, 返回耗时与重传次数.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <nrf24l01p.h>
#include <rf24.h>
#include <mod-can.h>
#include <mod-gpio.h>
#include <display.h>

LOG_MODULE_REGISTER(rf24_radio, LOG_LEVEL_INF);

#define RF24_PAYLOAD_MAX  32
#define RF24_ID_SIZE      2
#define RF24_TX_TIMEOUT   K_MSEC(100)

static const struct device *rf24_dev = DEVICE_DT_GET(DT_NODELABEL(nrf24));
static K_MUTEX_DEFINE(rf24_tx_mutex); /* 序列化多线程 TX (adc + gpio 按键) */

/* RX 帧 msgq: nRF24 驱动 IRQ 线程投递, 本模块 RX 线程消费 */
K_MSGQ_DEFINE(rf24_rx_msgq, sizeof(struct nrf24_frame), 8, 4);

void rf24_init(void)
{
	if (!device_is_ready(rf24_dev)) {
		LOG_ERR("nRF24 device not ready");
		return;
	}
	/* 注册 msgq: 驱动 IRQ 线程收到帧后投递到这里 */
	nrf24_add_rx_msgq(rf24_dev, &rf24_rx_msgq);
	/* 进入 PRX 接收模式并拉高 CE */
	int ret = nrf24_start_rx(rf24_dev);

	if (ret != 0) {
		LOG_ERR("nRF24 start RX failed: %d", ret);
		return;
	}
	LOG_INF("nRF24L01+ ready (PRX, irq-driven rx)");
}

void rf24_deinit(void)
{
	if (!device_is_ready(rf24_dev)) {
		return;
	}
	nrf24_set_mode(rf24_dev, NRF24_MODE_POWER_DOWN);
	LOG_INF("nRF24L01+ powered down");
}

bool rf24_data_send(uint16_t can_id, const uint8_t *data, size_t len)
{
	if (!device_is_ready(rf24_dev)) {
		return false;
	}
	if (len > RF24_PAYLOAD_MAX - RF24_ID_SIZE) {
		LOG_ERR("Payload too large: %zu (max %d)", len, RF24_PAYLOAD_MAX - RF24_ID_SIZE);
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
		LOG_WRN("nRF24 send failed (id=0x%03x ret=%d acked=%d retrans=%d)",
			can_id, ret, result.acked, result.retransmits);
		return false;
	}
	if (global_params.log) {
		LOG_INF("TX id=0x%03x acked=%d %ums retrans=%d", can_id, result.acked,
			result.elapsed_ms, result.retransmits);
	}
	return true;
}

bool rf24_get_link_status(void)
{
	return true;
}

/* ================================================================
 * 2.4G 接收线程 — 从 rf24_rx_msgq 取帧, 按 CAN ID 分发
 *
 * 帧: [CAN ID 2B BE][payload]
 * 支持 OVERBREAK_LASER / COORD_XY / COORD_Z, 复用 mod_can_parse_scanner().
 * ================================================================ */
static void rf24_rx_thread(void)
{
	if (!device_is_ready(rf24_dev)) {
		LOG_ERR("nRF24 device not ready, RX thread exit");
		return;
	}

	struct nrf24_frame frame;

	while (true) {
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}
		/* 仅 2.4G 模式下接收 */
		k_event_wait(&global_params.event, RF24_EVENT, false, K_FOREVER);

		/* 阻塞等待驱动 IRQ 线程投递的帧 */
		if (k_msgq_get(&rf24_rx_msgq, &frame, K_MSEC(200)) != 0) {
			continue;
		}
		if (frame.len < RF24_ID_SIZE) {
			LOG_WRN("nRF24 frame too short: %u", frame.len);
			continue;
		}

		uint16_t can_id = sys_get_be16(frame.data);
		uint8_t data_len = frame.len - RF24_ID_SIZE;

		/* 仅处理扫描仪数据帧, 其它 ID 忽略 */
		if (can_id != OVERBREAK_LASER && can_id != COORD_XY && can_id != COORD_Z) {
			LOG_DBG("Ignore 2.4G frame id=0x%03x", can_id);
			continue;
		}

		struct can_frame cf = {
			.id = can_id,
			.dlc = can_bytes_to_dlc(data_len),
		};
		memcpy(cf.data, frame.data + RF24_ID_SIZE, data_len);
		mod_can_parse_scanner(&cf);
	}
}
K_THREAD_DEFINE(thread_rf24_rx, 1024, rf24_rx_thread, NULL, NULL, NULL, 8, 0, 0);
