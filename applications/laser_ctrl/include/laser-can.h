#ifndef __LASER_CAN_H__
#define __LASER_CAN_H__
#include "laser-common.h"

enum {
	COB_ID1_RX = 0x664,
	COB_ID1_TX = 0x5E4,
	COB_ID2_RX = 0x665,
	COB_ID2_TX = 0x5E5,

	PLATFORM_RX   = 0x101,
	PLATFORM_TX   = 0x102,
	FW_DATA_RX    = 0x103,
};

int laser_can_send(struct can_frame *frame);
int cob_id664_process(struct can_frame *frame);
int cob_id665_process(struct can_frame *frame);

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>

struct image_fw_msg {
	uint32_t total_size;
	uint32_t offset;
	struct flash_img_context flash_img_ctx;
	uint8_t data[512];
};
int fw_update(struct can_frame *frame);
bool check_can_device_ready(void);
int laser_can_init(void);

#endif
