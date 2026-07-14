/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级模块 - 与 mod_handler 协议完全一致
 * CAN ID: PLATFORM_RX(0x101), PLATFORM_TX(0x102), FW_DATA_RX(0x103)
 * 字节序: 本机字节序 (ARM Cortex-M 小端), 使用 data_32 直接访问
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_fw, LOG_LEVEL_INF);

extern const struct device *can_dev;

/* 固件升级命令/响应码 - 与 mod_handler 一致 */
enum fw_error_code {
	FW_CODE_OFFSET,
	FW_CODE_UPDATE_SUCCESS,
	FW_CODE_VERSION,
	FW_CODE_CONFIRM,
	FW_CODE_FLASH_ERROR,
	FW_CODE_TRANFER_ERROR,
};

enum board_option {
	BOARD_START_UPDATE,
	BOARD_CONFIRM,
	BOARD_VERSION,
	BOARD_REBOOT,
};

#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)

/* ================================================================
 * CAN 固件响应 - 使用 data_32 本机字节序, 与 mod_handler 一致
 * ================================================================ */
static void fw_can_reply(uint32_t code, uint32_t offset)
{
	struct can_frame frame = {
		.id = PLATFORM_TX,
		.data_32 = {code, offset},
		.dlc = can_bytes_to_dlc(8),
	};

	can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
}

/* ================================================================
 * 固件升级处理 - 与 mod_handler 协议完全一致
 * ================================================================ */
static struct flash_img_context flash_img_ctx;
static bool fw_img_initialized = false;

int fw_update(struct can_frame *frame)
{
	uint32_t size = can_dlc_to_bytes(frame->dlc);

	if (frame->id == PLATFORM_RX) {
		/* 使用 data_32 直接访问, 与 mod_handler 一致 */
		if (frame->data_32[0] == BOARD_START_UPDATE) {
			/* 开始升级: [cmd 4B][total_size 4B] */
			if (size != 8) {
				LOG_ERR("start update: invalid size %d", size);
				fw_can_reply(FW_CODE_FLASH_ERROR, 0);
				return -1;
			}

			if (!fw_img_initialized) {
				const struct flash_area *fa;

				if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
					LOG_ERR("flash_area_open failed");
					fw_can_reply(FW_CODE_FLASH_ERROR, 0);
					return -1;
				}
				flash_area_erase(fa, 0, fa->fa_size);
				flash_area_close(fa);

				if (flash_img_init(&flash_img_ctx) != 0) {
					LOG_ERR("flash_img_init failed");
					fw_can_reply(FW_CODE_FLASH_ERROR, 0);
					return -1;
				}
				fw_img_initialized = true;
			}

			uint32_t total_size = frame->data_32[1];

			LOG_INF("FW upgrade start, size=%d", total_size);
			fw_can_reply(FW_CODE_OFFSET, 0);

		} else if (frame->data_32[0] == BOARD_CONFIRM) {
			/* 确认完成 */
			flash_img_buffered_write(&flash_img_ctx, NULL, 0, true);
			fw_img_initialized = false;

			if (flash_img_bytes_written(&flash_img_ctx) == 0) {
				LOG_ERR("FW upgrade failed: no data written");
				fw_can_reply(FW_CODE_TRANFER_ERROR, 0);
				return -1;
			}

			LOG_INF("FW upgrade complete, rebooting...");
			fw_can_reply(FW_CODE_CONFIRM, 0x55AA55AA);
			k_msleep(500);
			sys_reboot(SYS_REBOOT_COLD);

		} else if (frame->data_32[0] == BOARD_VERSION) {
			/* 查询版本 */
			fw_can_reply(FW_CODE_VERSION, 0x010000);

		} else if (frame->data_32[0] == BOARD_REBOOT) {
			/* 重启 */
			sys_reboot(SYS_REBOOT_WARM);
		}

	} else if (frame->id == FW_DATA_RX) {
		/* 固件数据 */
		if (!fw_img_initialized) {
			LOG_WRN("FW data before start");
			return -1;
		}

		if (flash_img_buffered_write(&flash_img_ctx, frame->data, size, false) != 0) {
			LOG_ERR("flash write failed");
			fw_can_reply(FW_CODE_FLASH_ERROR, 0);
			return -1;
		}

		/* 每 64 字节回复进度, 与 mod_handler 一致 */
		size_t written = flash_img_bytes_written(&flash_img_ctx);

		if (written % 64 == 0) {
			fw_can_reply(FW_CODE_OFFSET, written);
		}
	}

	return 0;
}
