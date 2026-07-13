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
#include <zephyr/logging/log.h>
#include <common.h>
#include <mod-can.h>
#include <rf24.h>

LOG_MODULE_REGISTER(common_link, LOG_LEVEL_INF);

/* 手柄状态帧 (HANDLER_STATE=0x1E3)：[x 2B BE][y 2B BE][btn][0xFF...]
 * CAN 用 8 字节 dlc，RF24 用 7 字节 payload（无 CAN 的末字节 0xFF）。
 */
int send_handler_state(const global_params_t *params)
{
	if (params->connect_type == CAN_TYPE) {
		struct can_frame frame = {
			.id = HANDLER_STATE,
			.dlc = can_bytes_to_dlc(8),
		};
		sys_put_be16((uint16_t)params->x_degree, &frame.data[0]);
		sys_put_be16((uint16_t)params->y_degree, &frame.data[2]);
		frame.data[4] = params->h_button;
		frame.data[5] = 0xFF;
		frame.data[6] = 0xFF;
		frame.data[7] = 0xFF;
		if (params->log) {
			LOG_INF("x: %d, y: %d, button: %d",
				params->x_degree, params->y_degree, params->h_button);
		}
		return mod_can_send(&frame);
	} else {
		uint8_t payload[7];
		sys_put_be16((uint16_t)params->x_degree, &payload[0]);
		sys_put_be16((uint16_t)params->y_degree, &payload[2]);
		payload[4] = params->h_button;
		payload[5] = 0xFF;
		payload[6] = 0xFF;
		return rf24_data_send(HANDLER_STATE, payload, sizeof(payload)) ? 0 : -EIO;
	}
}
