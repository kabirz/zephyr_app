/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级库 - 通用实现
 * 库内部管理 CAN 过滤器，应用只需调用 can_fw_rx_handler
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

/* 内部状态 */
static struct flash_img_context flash_img_ctx;
static bool fw_img_initialized = false;

/* ================================================================
 * CAN 固件响应
 * ================================================================ */
static void fw_can_reply(struct can_fw_ctx *ctx, uint32_t code, uint32_t offset)
{
	if (!ctx || !ctx->send_func) {
		return;
	}

	struct can_frame frame = {
		.id = CAN_FW_PLATFORM_TX,
		.data_32 = {code, offset},
		.dlc = can_bytes_to_dlc(8),
	};

	ctx->send_func(&frame);
}

/* ================================================================
 * API: 初始化
 * ================================================================ */
int can_fw_init(struct can_fw_ctx *ctx, const struct device *dev,
		can_fw_send_t send)
{
	if (!ctx || !dev || !send) {
		return -EINVAL;
	}

	ctx->send_func = send;
	ctx->can_dev = dev;
	fw_img_initialized = false;

	/* 注册过滤器 */
	ctx->filter.mask = CAN_STD_ID_MASK;

	ctx->filter.id = CAN_FW_PLATFORM_RX;
	can_add_rx_filter_msgq(dev, NULL, &ctx->filter);

	ctx->filter.id = CAN_FW_FW_DATA_RX;
	can_add_rx_filter_msgq(dev, NULL, &ctx->filter);

	LOG_INF("CAN FW upgrade initialized, version=0x%08x", APPVERSION);
	return 0;
}

/* ================================================================
 * API: 查询帧是否匹配
 * ================================================================ */
bool can_fw_is_upgrade_frame(struct can_frame *frame)
{
	if (!frame) {
		return false;
	}
	return frame->id == CAN_FW_PLATFORM_RX || frame->id == CAN_FW_FW_DATA_RX;
}

/* ================================================================
 * API: CAN 帧处理
 * ================================================================ */
bool can_fw_rx_handler(struct can_fw_ctx *ctx, struct can_frame *frame)
{
	if (!ctx || !frame) {
		return false;
	}

	if (frame->id == CAN_FW_PLATFORM_RX) {
		uint32_t cmd = frame->data_32[0];

		if (cmd == FW_CMD_START_UPDATE) {
			uint32_t size = can_dlc_to_bytes(frame->dlc);

			if (size != 8) {
				LOG_ERR("start update: invalid size %d", size);
				fw_can_reply(ctx, FW_CODE_FLASH_ERROR, 0);
				return true;
			}

			if (!fw_img_initialized) {
				const struct flash_area *fa;

				if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
					LOG_ERR("flash_area_open failed");
					fw_can_reply(ctx, FW_CODE_FLASH_ERROR, 0);
					return true;
				}
				flash_area_erase(fa, 0, fa->fa_size);
				flash_area_close(fa);

				if (flash_img_init(&flash_img_ctx) != 0) {
					LOG_ERR("flash_img_init failed");
					fw_can_reply(ctx, FW_CODE_FLASH_ERROR, 0);
					return true;
				}
				fw_img_initialized = true;
			}

			LOG_INF("FW upgrade start, size=%d", frame->data_32[1]);
			fw_can_reply(ctx, FW_CODE_OFFSET, 0);

		} else if (cmd == FW_CMD_CONFIRM) {
			flash_img_buffered_write(&flash_img_ctx, NULL, 0, true);
			fw_img_initialized = false;

			if (flash_img_bytes_written(&flash_img_ctx) == 0) {
				LOG_ERR("FW upgrade failed: no data written");
				fw_can_reply(ctx, FW_CODE_TRANFER_ERROR, 0);
				return true;
			}

			LOG_INF("FW upgrade complete, rebooting...");
			fw_can_reply(ctx, FW_CODE_CONFIRM, 0x55AA55AA);
			k_msleep(500);
			sys_reboot(SYS_REBOOT_COLD);

		} else if (cmd == FW_CMD_VERSION) {
			fw_can_reply(ctx, FW_CODE_VERSION, APPVERSION);

		} else if (cmd == FW_CMD_REBOOT) {
			sys_reboot(SYS_REBOOT_WARM);
		}

		return true;

	} else if (frame->id == CAN_FW_FW_DATA_RX) {
		if (!fw_img_initialized) {
			LOG_WRN("FW data before start");
			return true;
		}

		uint32_t size = can_dlc_to_bytes(frame->dlc);

		if (flash_img_buffered_write(&flash_img_ctx, frame->data, size, false) != 0) {
			LOG_ERR("flash write failed");
			fw_can_reply(ctx, FW_CODE_FLASH_ERROR, 0);
			return true;
		}

		/* 每 64 字节回复进度 */
		size_t written = flash_img_bytes_written(&flash_img_ctx);

		if (written % 64 == 0) {
			fw_can_reply(ctx, FW_CODE_OFFSET, written);
		}

		return true;
	}

	return false;
}