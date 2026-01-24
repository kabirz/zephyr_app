#ifndef __MOD_CAN_H__
#define __MOD_CAN_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>

enum {
	PLATFORM_RX   = 0x101,
	PLATFORM_TX   = 0x102,
	FW_DATA_RX    = 0x103,
};

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

enum {
	LASER_DEVICE_STATUS,
	LASER_WRITE_MODE,
	LASER_NEED_CLOSE,
	LASER_ON,
	LASER_CON_MESURE,
	LASER_FW_UPDATE,
};

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>

struct image_fw_msg {
	uint32_t total_size;
	uint32_t offset;
	struct flash_img_context flash_img_ctx;
};
int fw_update(struct can_frame *frame);
int mod_can_send(struct can_frame *frame);
#endif
