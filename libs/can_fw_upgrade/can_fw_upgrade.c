/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级库 - 自包含实现
 * 库通过 SYS_INIT 自动初始化 CAN (bitrate/start + 全接收过滤器),
 * 使用静态 K_THREAD_DEFINE 的 RX 线程, 内部处理固件升级;
 * 非固件帧通过应用注册的回调分发。
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include "can_fw_upgrade.h"

LOG_MODULE_REGISTER(can_fw_upgrade, LOG_LEVEL_INF);

/* CAN 帧 ID */
#define CAN_FW_PLATFORM_RX  0x101
#define CAN_FW_PLATFORM_TX  0x102
#define CAN_FW_FW_DATA_RX   0x103

/* 命令码 */
enum fw_cmd {
	FW_CMD_START_UPDATE = 0,
	FW_CMD_CONFIRM,
	FW_CMD_VERSION,
	FW_CMD_REBOOT,
};

/* 响应码 */
enum fw_code {
	FW_CODE_OFFSET = 0,
	FW_CODE_UPDATE_SUCCESS,
	FW_CODE_VERSION,
	FW_CODE_CONFIRM,
	FW_CODE_FLASH_ERROR,
	FW_CODE_TRANFER_ERROR,
};

#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)

/* ================================================================
 * 全局状态
 * ================================================================ */
static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* 已注册的业务帧 handler 列表 (RX 线程按序遍历广播) */
struct can_fw_handler {
	can_fw_app_rx_cb_t cb;
	void *user_data;
};
static struct can_fw_handler handlers[CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS];

static struct flash_img_context flash_img_ctx;
static bool fw_img_initialized;

/* RX msgq (全接收过滤器投递目标) */
K_MSGQ_DEFINE(can_fw_rx_msgq, sizeof(struct can_frame), 8, 4);

/* ================================================================
 * 固件升级响应
 * ================================================================ */
static void fw_can_reply(uint32_t code, uint32_t offset)
{
	struct can_frame frame = {
		.id = CAN_FW_PLATFORM_TX,
		.data_32 = {code, offset},
		.dlc = can_bytes_to_dlc(8),
	};

	can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
}

/* ================================================================
 * 固件控制帧处理 (0x101)
 * ================================================================ */
static void handle_platform_rx(struct can_frame *frame)
{
	uint32_t cmd = frame->data_32[0];

	if (cmd == FW_CMD_START_UPDATE) {
		uint32_t size = can_dlc_to_bytes(frame->dlc);

		if (size != 8) {
			LOG_ERR("start update: invalid size %d", size);
			fw_can_reply(FW_CODE_FLASH_ERROR, 0);
			return;
		}

		if (!fw_img_initialized) {
			const struct flash_area *fa;

			if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
				LOG_ERR("flash_area_open failed");
				fw_can_reply(FW_CODE_FLASH_ERROR, 0);
				return;
			}
			flash_area_erase(fa, 0, fa->fa_size);
			flash_area_close(fa);

			if (flash_img_init(&flash_img_ctx) != 0) {
				LOG_ERR("flash_img_init failed");
				fw_can_reply(FW_CODE_FLASH_ERROR, 0);
				return;
			}
			fw_img_initialized = true;
		}

		LOG_INF("FW upgrade start, size=%d", frame->data_32[1]);
		fw_can_reply(FW_CODE_OFFSET, 0);

	} else if (cmd == FW_CMD_CONFIRM) {
		flash_img_buffered_write(&flash_img_ctx, NULL, 0, true);
		fw_img_initialized = false;

		if (flash_img_bytes_written(&flash_img_ctx) == 0) {
			LOG_ERR("FW upgrade failed: no data written");
			fw_can_reply(FW_CODE_TRANFER_ERROR, 0);
			return;
		}

		LOG_INF("FW upgrade complete, rebooting...");
		fw_can_reply(FW_CODE_CONFIRM, 0x55AA55AA);
		k_msleep(500);
		sys_reboot(SYS_REBOOT_COLD);

	} else if (cmd == FW_CMD_VERSION) {
		fw_can_reply(FW_CODE_VERSION, APPVERSION);

	} else if (cmd == FW_CMD_REBOOT) {
		sys_reboot(SYS_REBOOT_WARM);
	}
}

/* ================================================================
 * 固件数据帧处理 (0x103)
 * ================================================================ */
static void handle_fw_data(struct can_frame *frame)
{
	if (!fw_img_initialized) {
		LOG_WRN("FW data before start");
		return;
	}

	uint32_t size = can_dlc_to_bytes(frame->dlc);

	if (flash_img_buffered_write(&flash_img_ctx, frame->data, size, false) != 0) {
		LOG_ERR("flash write failed");
		fw_can_reply(FW_CODE_FLASH_ERROR, 0);
		return;
	}

	/* 每 64 字节回复进度 */
	size_t written = flash_img_bytes_written(&flash_img_ctx);

	if (written % 64 == 0) {
		fw_can_reply(FW_CODE_OFFSET, written);
	}
}

/* ================================================================
 * RX 线程 (静态): 固件帧内部处理, 其余帧分发应用回调
 * ================================================================ */
static void can_fw_rx_thread_fn(void *p1, void *p2, void *p3)
{
	struct can_frame frame;

	while (1) {
		if (k_msgq_get(&can_fw_rx_msgq, &frame, K_FOREVER) != 0) {
			continue;
		}

		if (frame.id == CAN_FW_PLATFORM_RX) {
			handle_platform_rx(&frame);
		} else if (frame.id == CAN_FW_FW_DATA_RX) {
			handle_fw_data(&frame);
		} else {
			/* 广播给所有已注册的业务帧 handler; 若均未处理则告警 */
			bool handled = false;

			for (int i = 0; i < CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS; i++) {
				if (handlers[i].cb && handlers[i].cb(&frame, handlers[i].user_data)) {
					handled = true;
				}
			}
			if (!handled) {
				uint8_t dlc = can_dlc_to_bytes(frame.dlc);

				LOG_WRN("unhandled CAN frame id=0x%03x dlc=%u", frame.id, dlc);
				LOG_HEXDUMP_WRN(frame.data, dlc, "data");
			}
		}
	}
}

K_THREAD_DEFINE(can_fw_rx_thread, CONFIG_CAN_FW_UPGRADE_RX_STACK_SIZE,
		can_fw_rx_thread_fn, NULL, NULL, NULL,
		CONFIG_CAN_FW_UPGRADE_RX_PRIORITY, 0, 0);

/* ================================================================
 * SYS_INIT: 初始化 CAN (bitrate/start + 全接收过滤器)
 * RX 线程由 K_THREAD_DEFINE 静态创建, 启动后阻塞在 msgq 等待帧。
 * ================================================================ */
static int can_fw_init(void)
{
	int err;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");
		return -ENODEV;
	}

	err = can_set_bitrate(can_dev, CONFIG_CAN_FW_UPGRADE_BITRATE);
	if (err) {
		LOG_ERR("CAN set bitrate failed: %d", err);
		return err;
	}
	err = can_start(can_dev);
	if (err) {
		LOG_ERR("CAN start failed: %d", err);
		return err;
	}

	/* 全接收过滤器 (mask=0): 所有 CAN 帧进入库 msgq */
	static const struct can_filter filter = {.mask = 0};

	can_add_rx_filter_msgq(can_dev, &can_fw_rx_msgq, &filter);

	LOG_INF("CAN FW upgrade initialized, version=0x%08x", APPVERSION);
	return 0;
}
SYS_INIT(can_fw_init, APPLICATION, CONFIG_CAN_FW_UPGRADE_INIT_PRIORITY);

/* ================================================================
 * API: 注册业务帧回调
 * ================================================================ */
const struct device *can_fw_set_app_handler(can_fw_app_rx_cb_t cb, void *user_data)
{
	if (cb == NULL) {
		return can_dev;
	}
	for (int i = 0; i < CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS; i++) {
		if (handlers[i].cb == NULL) {
			handlers[i].cb = cb;
			handlers[i].user_data = user_data;
			return can_dev;
		}
	}
	LOG_WRN("handler array full (%d)", CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS);
	return NULL;
}

int can_fw_remove_handler(can_fw_app_rx_cb_t cb)
{
	for (int i = 0; i < CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS; i++) {
		if (handlers[i].cb == cb) {
			handlers[i].cb = NULL;
			handlers[i].user_data = NULL;
			return 0;
		}
	}
	return -ENOENT;
}
