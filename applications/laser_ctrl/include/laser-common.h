#ifndef __LASER_COMMON_H__
#define __LASER_COMMON_H__
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>

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

struct laser_can_msg {
	struct can_frame frame;
	#define CAN_RX       0
	#define CAN_SEND     1
	#define CAN_TX_COM   2
	uint8_t type;
	int error;
	int count;
};
enum {
	LASER_WRITE_MODE,
	LASER_ON,
	LASER_CON_MESURE,
	LASER_FW_UPDATE,
};
extern atomic_t laser_status;


#endif
