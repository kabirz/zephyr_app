/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 总线收发 + 心跳线程 + 手柄状态上报 (0x1E3 BE)
 * + 扫描仪数据解析 (0x263/0x363/0x463)
 */

#include <zephyr/kernel.h>
#include <mod-can.h>
#include <display.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <common.h>
LOG_MODULE_REGISTER(mod_can, LOG_LEVEL_INF);

static const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(mod_can_msgq, 8);

static void mod_canrx_msg_handler(struct can_frame *frame)
{
	switch (frame->id) {
	case PLATFORM_RX:
	case FW_DATA_RX:
		fw_update(frame);
		break;
	case OVERBREAK_LASER:
	case COORD_XY:
	case COORD_Z:
		mod_can_parse_scanner(frame);
		break;
	default:
		LOG_ERR("can frame id (0x%x) is not support", frame->id);
	}
}

static void mod_cantx_callback(const struct device *dev, int error, void *user_data)
{
	uint32_t count = *(uint32_t *)user_data;
	if (error == 0) {
		LOG_DBG("CAN frame #%u successfully sent", count);
	} else {
		LOG_ERR("failed to send CAN frame #%u (err %d)", count, error);
	}
}

int mod_can_send(struct can_frame *frame)
{
	static uint32_t frame_count = 0;

	frame_count++;

	return can_send(can_dev, frame, K_MSEC(100), mod_cantx_callback, &frame_count);
}

bool check_can_device_ready(void)
{
	return DEVICE_API_IS(can, can_dev) && device_is_ready(can_dev);
}

int mod_can_init(void)
{
	int err = -1;
	uint8_t can_check_time = 0;

	while (can_check_time++ < 10) {
		if (check_can_device_ready()) {
			break;
		}
		k_msleep(10);
	}
	if (can_check_time >= 10) {
		LOG_ERR("can device is not ready");
		goto end;
	}

	if ((err = can_set_bitrate(can_dev, 250000)) != 0) {
		LOG_ERR("failed to set bitrate (err %d)", err);
		goto end;
	}

	if ((err = can_start(can_dev)) != 0) {
		LOG_ERR("failed to start CAN controller (err %d)", err);
		goto end;
	}

	struct can_filter filter = {.mask = CAN_STD_ID_MASK};

	filter.id = PLATFORM_RX;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	filter.id = FW_DATA_RX;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	filter.id = OVERBREAK_LASER;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	filter.id = COORD_XY;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	filter.id = COORD_Z;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);
end:
	return err;
}

void mod_can_process_thread(void)
{
	struct can_frame frame;

	if (mod_can_init() != 0) {
		LOG_ERR("can init failed");
		return;
	}

	while (true) {
		if (k_msgq_get(&mod_can_msgq, &frame, K_FOREVER) == 0) {
			mod_canrx_msg_handler(&frame);
			if (!k_event_test(&global_params.event, CAN_RX_EVENT)) {
				k_event_post(&global_params.event, CAN_RX_EVENT);
			}
		}
	}
}

K_THREAD_DEFINE(thread_can_rx, 2048, mod_can_process_thread, NULL, NULL, NULL, 8, 0, 0);

/* ================================================================
 * 扫描仪 CAN 数据解析 — 0x263/0x363/0x463, 大端序
 * ================================================================ */
void mod_can_parse_scanner(struct can_frame *frame)
{
	scanner_data_t *s = &global_params.scanner;

	switch (frame->id) {
	case OVERBREAK_LASER:
		/* Byte 0: flags (bit0=overbreak_valid, bit1=laser_valid)
		 * Byte 1: reserved
		 * Byte 2-3: overbreak_value (int16_t BE)
		 * Byte 4-7: laser_value (uint32_t BE)
		 */
		s->overbreak_valid = (frame->data[0] & 0x01) ? 1 : -1;
		s->laser_valid = (frame->data[0] & 0x02) ? 1 : -1;
		s->overbreak_value = (int16_t)sys_get_be16(&frame->data[2]);
		s->laser_distance = (int32_t)sys_get_be32(&frame->data[4]);
		if (global_params.log) {
			LOG_INF("Overbreak: valid=%d val=%d, Laser: valid=%d val=%d", s->overbreak_valid,
				s->overbreak_value, s->laser_valid, s->laser_distance);
		}
		break;

	case COORD_XY:
		/* Byte 0-3: coordX (int32_t BE)
		 * Byte 4-7: coordY (int32_t BE)
		 */
		s->coord_x = (int32_t)sys_get_be32(&frame->data[0]);
		s->coord_y = (int32_t)sys_get_be32(&frame->data[4]);
		s->coord_xy_valid = 1;
		if (global_params.log) {
			LOG_INF("CoordXY: X=%d Y=%d", s->coord_x, s->coord_y);
		}
		break;

	case COORD_Z:
		/* Byte 0-3: coordZ (int32_t BE)
		 * Byte 4: flags (bit0: coordz_valid)
		 * Byte 5-7: reserved
		 *
		 * Z 坐标有效标志同时设置 X/Y/Z 三个坐标的有效性
		 */
		s->coord_z = (int32_t)sys_get_be32(&frame->data[0]);
		s->coord_z_valid = (frame->data[4] & 0x01) ? 1 : -1;
		s->coord_xy_valid = s->coord_z_valid;
		if (global_params.log) {
			LOG_INF("CoordZ: Z=%d valid=%d", s->coord_z, s->coord_z_valid);
		}
		break;

	default:
		break;
	}

	/* 解析完成后刷新扫描仪数据显示 */
	mod_display_scanner(s);
}
