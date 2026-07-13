/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 通用链路发送：按 connect_type 分派 CAN / 2.4G (nRF24L01+)。
 * send_handler_state 由事件驱动调用点 (gpio/adc) 与周期心跳线程复用。
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <common.h>
#include <mod-can.h>
#include <rf24.h>

LOG_MODULE_REGISTER(common_link, LOG_LEVEL_INF);

/* 手柄状态帧 (HANDLER_STATE=0x1E3)：[x 2B BE][y 2B BE][btn][0xFF...]
 * CAN 用 8 字节 dlc，RF24 用 7 字节 payload（无 CAN 的末字节 0xFF）。
 */
int send_handler_state(const uint8_t *data, uint8_t len)
{
	if (global_params.connect_type == CAN_TYPE) {
		struct can_frame frame = {
			.id = HANDLER_STATE,
			.dlc = can_bytes_to_dlc(len),
		};
		memcpy(frame.data, data, len);
		return mod_can_send(&frame);
	} else {
		return rf24_data_send(HANDLER_STATE, data, len) ? 0 : -EIO;
	}
}

/* 统一心跳：周期上报手柄状态，按 connect_type 自动分派 CAN/RF24。
 * 等 CAN_EVENT 或 RF24_EVENT 任一置位（connect_switch 切换时置位）。
 */
static void heart_thread(void)
{
	while (true) {
		k_event_wait(&global_params.event, CAN_EVENT | RF24_EVENT, false, K_FOREVER);
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}
		uint8_t buf[8];
		sys_put_be16((uint16_t)global_params.x_degree, &buf[0]);
		sys_put_be16((uint16_t)global_params.y_degree, &buf[2]);
		buf[4] = global_params.h_button;
		uint8_t len;
		if (global_params.connect_type == CAN_TYPE) {
			buf[5] = buf[6] = buf[7] = 0xFF;
			len = 8;
		} else {
			buf[5] = buf[6] = 0xFF;
			len = 7;
		}
		if (global_params.log) {
			LOG_INF("x: %d, y: %d, button: %d",
				global_params.x_degree, global_params.y_degree, global_params.h_button);
		}
		send_handler_state(buf, len);
		k_sleep(K_MSEC(global_params.report_period));
	}
}
K_THREAD_DEFINE(thread_heart, 1024, heart_thread, NULL, NULL, NULL, 11, 0, 0);
