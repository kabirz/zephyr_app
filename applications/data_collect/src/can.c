/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 业务侧: 固件升级由 can_fw_upgrade 库自管 (0x101-0x105, 含 keyhash 与
 * 版本字符串), 本模块仅:
 *   - 注册业务帧回调: 处理 CAN 参数配置帧 DC_CAN_CFG_CMD (0x1A0), 复用
 *     dc_build_config_payload() 施加参数设置并回 DC_CAN_CFG_RESP (0x1A1);
 *   - 周期上报状态帧 DC_CAN_HEARTBEAT (0x763), 供上位机确认节点存活.
 *
 * CAN 配置命令帧 (0x1A0):   [sub 1B][payload ≤7B]    (sub 复用 enum udp_cmd)
 * CAN 配置响应帧 (0x1A1):   [sub 1B][seq 1B][payload ≤6B]  (payload>6B 分帧)
 */

#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <data_collect.h>
#include <can_fw_upgrade.h>
#define DC_NO_MODBUS_LOG_MODULE
#include "modbus/init.h"

LOG_MODULE_REGISTER(dc_can, LOG_LEVEL_INF);

static const struct device *can_dev;

/* ================================================================
 * CAN 发送
 * ================================================================ */
static void dc_cantx_callback(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (error) {
		LOG_ERR("CAN tx error: %d", error);
	}
}

static int dc_can_send(struct can_frame *frame)
{
	if (!can_dev) {
		return -ENODEV;
	}
	return can_send(can_dev, frame, K_MSEC(100), dc_cantx_callback, NULL);
}

/* ================================================================
 * 周期状态帧 0x763: [version 2B BE][di 2B BE][do 1B][reserved 3B]
 * ================================================================ */
static void dc_can_heart_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct can_frame frame = {
		.id = DC_CAN_HEARTBEAT,
		.dlc = can_bytes_to_dlc(8),
	};

	while (1) {
		k_sleep(K_SECONDS(1));

		uint16_t ver = (APP_VERSION_MAJOR << 8) | APP_VERSION_MINOR;
		uint16_t di = get_input_reg(INPUT_DI_IDX);

		sys_put_be16(ver, frame.data);
		sys_put_be16(di, frame.data + 2);
		frame.data[4] = get_holding_reg(HOLDING_DO_IDX) & 0xff;
		memset(frame.data + 5, 0, 3);

		if (dc_can_send(&frame) != 0) {
			LOG_DBG("can heartbeat send failed");
		}
	}
}

K_THREAD_DEFINE(dc_can_heart, 512, dc_can_heart_thread, NULL, NULL, NULL, 12, 0, 0);

/* ================================================================
 * 配置响应分帧发送 (0x1A1): [sub 1][seq 1][payload ≤6B]
 * ================================================================ */
static void can_send_config_resp(uint8_t sub, const uint8_t *payload, int len)
{
	uint8_t off = 0;
	uint8_t seq = 0;

	do {
		uint8_t chunk = MIN(len - off, 6);
		struct can_frame frame = {
			.id = DC_CAN_CFG_RESP,
			.dlc = can_bytes_to_dlc(8),
		};

		frame.data[0] = sub;
		frame.data[1] = seq;
		memcpy(&frame.data[2], payload + off, chunk);
		memset(&frame.data[2 + chunk], 0, 6 - chunk);
		dc_can_send(&frame);
		off += chunk;
		seq++;
	} while (off < len);
}

/* ================================================================
 * 业务帧回调 (库 RX 线程调用)
 * ================================================================ */
static bool dc_can_app_rx(struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(user_data);

	if (frame->id == DC_CAN_CFG_CMD) {
		uint8_t sub = frame->data[0];
		uint8_t req[7];
		uint8_t resp[8];

		if (can_dlc_to_bytes(frame->dlc) < 2) {
			return true;
		}
		uint8_t req_len = MIN(can_dlc_to_bytes(frame->dlc) - 1, sizeof(req));

		memcpy(req, &frame->data[1], req_len);
		int rlen = dc_build_config_payload(sub, req, req_len, resp, sizeof(resp));

		if (rlen >= 0) {
			can_send_config_resp(sub, resp, rlen);
			LOG_INF("CAN cfg: sub=0x%02x rlen=%d", sub, rlen);
		} else {
			LOG_WRN("CAN cfg: unknown sub 0x%02x", sub);
		}
		return true;
	}

	return false;
}

static int dc_can_init(void)
{
	can_dev = can_fw_set_app_handler(dc_can_app_rx, NULL);
	return 0;
}

SYS_INIT(dc_can_init, APPLICATION, CONFIG_DC_CAN_INIT_PRIORITY);