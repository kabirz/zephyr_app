/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 总线收发 + 心跳线程 + 手柄状态上报 (0x1E3 BE)
 * + 扫描仪数据解析 (0x263/0x363/0x463)
 * + LoRa 远程配参 (0x105/0x106, k_work 异步)
 */

#include <zephyr/kernel.h>
#include <mod-can.h>
#include <lora.h>
#include <display.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <common.h>
LOG_MODULE_REGISTER(mod_can, LOG_LEVEL_INF);

static const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(mod_can_msgq, 8);

static atomic_t heart_send_success = ATOMIC_INIT(0);

static void can_lora_config_handler(struct can_frame *frame);

static void mod_canrx_msg_handler(struct can_frame *frame)
{
	switch (frame->id) {
	case PLATFORM_RX:
	case FW_DATA_RX:
		fw_update(frame);
		break;
	case LORA_CONFIG_RX:
		can_lora_config_handler(frame);
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

static void heart_tx_callback(const struct device *dev, int error, void *user_data)
{
	if (error == 0) {
		atomic_set(&heart_send_success, 1);
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

/* ================================================================
 * LoRa 参数配置 — CAN 远程设置/查询, k_work 异步执行
 *   lora_gw_configure() 涉及 AT 模式握手 + 模块重启 (10s+),
 *   不能在 CAN 接收线程中同步执行, 提交到系统工作队列
 * ================================================================ */
static struct k_work lora_cfg_work;
static struct lora_gw_config pending_lora_cfg;
static uint32_t pending_nid;
static uint32_t pending_gwid;
static uint16_t pending_ch;
static uint8_t lora_cfg_cmd;

static void lora_cfg_work_handler(struct k_work *work)
{
	struct can_frame resp = {
		.id = LORA_CONFIG_TX,
		.dlc = can_bytes_to_dlc(8),
	};

	if (lora_cfg_cmd == LORA_CMD_SET) {
		int ret = lora_gw_configure(&pending_lora_cfg);

		resp.data[0] = (ret == 0) ? LORA_CFG_OK : LORA_CFG_FAIL;
		LOG_INF("LoRa config SET: ret=%d prot=%d mode=%d spd=%d ch=%d", ret,
			pending_lora_cfg.prot, pending_lora_cfg.mode, pending_lora_cfg.spd,
			pending_lora_cfg.ch);
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY) {
		struct lora_gw_config cfg;
		int ret = lora_gw_query(&cfg);

		resp.data[0] = (ret == 0) ? LORA_CFG_OK : LORA_CFG_FAIL;
		if (ret == 0) {
			resp.data[1] = (uint8_t)((cfg.prot << 4) | (cfg.mode & 0x0F));
			resp.data[2] = cfg.spd;
			resp.data[3] = cfg.ch;
			LOG_INF("LoRa config QUERY: prot=%d mode=%d spd=%d ch=%d", cfg.prot,
				cfg.mode, cfg.spd, cfg.ch);
		} else {
			LOG_ERR("LoRa config QUERY failed: %d", ret);
		}
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY_NID) {
		uint32_t nid = global_params.nid;

		resp.data[0] = LORA_CFG_OK;
		sys_put_be32(nid, &resp.data[4]);
		LOG_INF("LoRa NID QUERY: nid=0x%08x", nid);
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_SET_NID) {
		int ret = lora_set_node_id(pending_nid);

		resp.data[0] = (ret == 0) ? LORA_CFG_OK : LORA_CFG_FAIL;
		if (ret == 0) {
			sys_put_be32(pending_nid, &resp.data[4]);
			LOG_INF("LoRa NID SET: nid=0x%08x", pending_nid);
		} else {
			LOG_ERR("LoRa NID SET failed: %d", ret);
		}
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY_GWID) {
		uint32_t gwid = global_params.gwid;

		resp.data[0] = LORA_CFG_OK;
		sys_put_be32(gwid, &resp.data[4]);
		LOG_INF("LoRa GWID QUERY: gwid=0x%08x", gwid);
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_SET_GWID) {
		int ret = lora_set_gw_id(pending_gwid);

		resp.data[0] = (ret == 0) ? LORA_CFG_OK : LORA_CFG_FAIL;
		if (ret == 0) {
			sys_put_be32(pending_gwid, &resp.data[4]);
			LOG_INF("LoRa GWID SET: gwid=0x%08x", pending_gwid);
		} else {
			LOG_ERR("LoRa GWID SET failed: %d", ret);
		}
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY_CH1) {
		resp.data[0] = LORA_CFG_OK;
		sys_put_be16(global_params.ch1, &resp.data[2]);
		LOG_INF("LoRa CH1 QUERY: ch1=%d", global_params.ch1);
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_SET_CH1) {
		int ret = lora_set_ch1(pending_ch);

		resp.data[0] = (ret == 0) ? LORA_CFG_OK : LORA_CFG_FAIL;
		if (ret == 0) {
			sys_put_be16(pending_ch, &resp.data[2]);
			LOG_INF("LoRa CH1 SET: ch1=%d", pending_ch);
		} else {
			LOG_ERR("LoRa CH1 SET failed: %d", ret);
		}
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY_CH2) {
		resp.data[0] = LORA_CFG_OK;
		sys_put_be16(global_params.ch2, &resp.data[2]);
		LOG_INF("LoRa CH2 QUERY: ch2=%d", global_params.ch2);
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_SET_CH2) {
		int ret = lora_set_ch2(pending_ch);

		resp.data[0] = (ret == 0) ? LORA_CFG_OK : LORA_CFG_FAIL;
		if (ret == 0) {
			sys_put_be16(pending_ch, &resp.data[2]);
			LOG_INF("LoRa CH2 SET: ch2=%d", pending_ch);
		} else {
			LOG_ERR("LoRa CH2 SET failed: %d", ret);
		}
		mod_can_send(&resp);
	}
}

static void can_lora_config_handler(struct can_frame *frame)
{
	lora_cfg_cmd = frame->data[0];

	if (lora_cfg_cmd == LORA_CMD_SET) {
		pending_lora_cfg.prot = (enum lora_gw_prot)(frame->data[1] >> 4);
		pending_lora_cfg.mode = (enum lora_gw_mode)(frame->data[1] & 0x0F);
		pending_lora_cfg.spd = frame->data[2];
		pending_lora_cfg.ch = frame->data[3];
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY) {
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY_NID) {
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_SET_NID) {
		pending_nid = sys_get_be32(&frame->data[4]);
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY_GWID) {
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_SET_GWID) {
		pending_gwid = sys_get_be32(&frame->data[4]);
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY_CH1) {
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_SET_CH1) {
		pending_ch = sys_get_be16(&frame->data[2]);
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY_CH2) {
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_SET_CH2) {
		pending_ch = sys_get_be16(&frame->data[2]);
		k_work_submit(&lora_cfg_work);
	} else {
		LOG_ERR("Unknown LoRa config cmd: 0x%02x", lora_cfg_cmd);
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

	filter.id = LORA_CONFIG_RX;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	filter.id = OVERBREAK_LASER;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	filter.id = COORD_XY;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	filter.id = COORD_Z;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	k_work_init(&lora_cfg_work, lora_cfg_work_handler);
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
			k_event_set(&global_params.event, CAN_RX_EVENT);
		}
	}
}

K_THREAD_DEFINE(mod_can, 2048, mod_can_process_thread, NULL, NULL, NULL, 11, 0, 0);

static void can_heart_thread(void)
{
	struct can_frame frame = {
		.data[0] = 5,
		.id = COBID_HEATBEAT,
		.dlc = can_bytes_to_dlc(1),
	};
	int fail_count = 0, ret;

	while (true) {
		k_event_wait(&global_params.event, CAN_EVENT, false, K_FOREVER);
		k_event_wait(&global_params.event, CAN_RX_EVENT, false, K_FOREVER);
		uint32_t t1 = k_uptime_get_32();
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}

		ret = can_send(can_dev, &frame, K_MSEC(100), heart_tx_callback, NULL);

		k_sleep(K_MSEC(50));

		if (atomic_get(&heart_send_success) == 0) {
			fail_count++;
			LOG_WRN("heartbeat send failed, count: %d", fail_count);
		}

		if (fail_count >= 3) {
			LOG_WRN("heartbeat failed 3 times");
			fail_count = 0;
			k_event_clear(&global_params.event, CAN_RX_EVENT);
			atomic_set(&heart_send_success, 0);
			continue;
		}
		uint32_t diff = k_uptime_get_32() - t1;

		if (diff < global_params.can_heart_time) {
			k_sleep(K_MSEC(global_params.can_heart_time - diff));
		}
	}
}

K_THREAD_DEFINE(can_heart, 1024, can_heart_thread, NULL, NULL, NULL, 11, 0, 0);

/* ================================================================
 * 手柄状态帧发送 — 0x1E3, 大端序, 8 字节
 *
 * Data[0-1]: coord_x (int16_t BE, 0.1° 单位)
 * Data[2-3]: coord_y (int16_t BE, 0.1° 单位)
 * Data[4]:   btn flags (bit0: btnHandler 反转, bit1: btnBox)
 * Data[5-7]: reserved (0xFF)
 * ================================================================ */
int mod_can_send_handler_state(const gloval_params_t *params)
{
	if (atomic_get(&heart_send_success) == 0) {
		return -1;
	}
	struct can_frame frame = {
		.id = HANDLER_STATE,
		.dlc = can_bytes_to_dlc(8),
	};

	/* 大端序写入角度 */
	sys_put_be16((uint16_t)params->x_degree, &frame.data[0]);
	sys_put_be16((uint16_t)params->y_degree, &frame.data[2]);

	/* 按键: btnHandler 反转逻辑 (按下=0, 松开=1) */
	frame.data[4] = params->h_button ? 0x00 : 0x01;
	frame.data[5] = 0xFF;
	frame.data[6] = 0xFF;
	frame.data[7] = 0xFF;

	return mod_can_send(&frame);
}

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
		LOG_DBG("Overbreak: valid=%d val=%d, Laser: valid=%d val=%d", s->overbreak_valid,
			s->overbreak_value, s->laser_valid, s->laser_distance);
		break;

	case COORD_XY:
		/* Byte 0-3: coordX (int32_t BE)
		 * Byte 4-7: coordY (int32_t BE)
		 */
		s->coord_x = (int32_t)sys_get_be32(&frame->data[0]);
		s->coord_y = (int32_t)sys_get_be32(&frame->data[4]);
		s->coord_xy_valid = 1;
		LOG_DBG("CoordXY: X=%d Y=%d", s->coord_x, s->coord_y);
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
		LOG_DBG("CoordZ: Z=%d valid=%d", s->coord_z, s->coord_z_valid);
		break;

	default:
		break;
	}

	/* 解析完成后刷新扫描仪数据显示 */
	mod_display_scanner(s);
}
