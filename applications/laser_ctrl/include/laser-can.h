#ifndef __LASER_CAN_H__
#define __LASER_CAN_H__
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>

enum {
	COB_ID1_RX = 0x664,
	COB_ID1_TX = 0x5E4,
	COB_ID2_RX = 0x665,
	COB_ID2_TX = 0x5E5,

	FW_UP_START   = 0x101,
	FW_UP_DATA    = 0x102,
	FW_UP_CONFIRM = 0x103,
	FW_GET_VER    = 0x104,
	FW_UP_TX      = 0x105,
};

enum fw_error_code {
	FW_CODE_OFFSET,
	FW_CODE_UPDATE_SUCCESS,
	FW_CODE_VERSION,
	FW_CODE_CONFIMR,
	FW_CODE_FLASH_ERROR,
	FW_CODE_TRANFER_ERROR,
};

struct laser_can_msg {
	struct can_frame frame;
	#define CAN_RX       0
	#define CAN_SEND     1
	#define CAN_TX_COM   2
	uint8_t type;
	int error;
	int count;
};
#define HEAP_SIZE (sizeof(struct laser_can_msg)*10)

struct can_cob_msg {
	uint8_t command;
	uint16_t index;
	uint8_t subindex;
	uint32_t data;
};
int laser_can_send(struct can_frame *frame);
int cob_id664_process(void *msg);
int cob_id665_process(void *msg);

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

#endif
