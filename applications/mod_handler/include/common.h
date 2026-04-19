
#ifndef _MOD_COMMON_H
#define _MOD_COMMON_H

#include <zephyr/kernel.h>

typedef enum {
	BATTERY_STATUS_UNKNOWN = 0,
	BATTERY_STATUS_FULL,
	BATTERY_STATUS_CHARGING,
	BATTERY_STATUS_NOT_CHARGING,
	BATTERY_STATUS_DISCHARGING,
} battery_status_t;

typedef struct {
	int8_t overbreak_valid;
	int32_t overbreak_value;
	int8_t laser_valid;
	int32_t laser_distance;
	int8_t coord_xy_valid;
	int32_t coord_x;
	int32_t coord_y;
	int8_t coord_z_valid;
	int32_t coord_z;
} scanner_data_t;

#define WAKE_EVENT     BIT(0)
#define CAN_RX_EVENT   BIT(1)
#define CAN_EVENT      BIT(2)
#define LORA_EVENT     BIT(3)

typedef struct {
	int x_degree;
	int y_degree;
	uint8_t h_button;
	uint8_t power_level;
#define CAN_TYPE  1
#define LORA_TYPE 2
	uint8_t connect_type;
	uint32_t can_heart_time;
	battery_status_t battery_status;
	scanner_data_t scanner;
	uint8_t rssi;
	uint32_t gwid;
	uint32_t nid;
	volatile bool sleeping;
	struct k_event event;
} gloval_params_t;

extern gloval_params_t global_params;
extern volatile uint32_t last_activity_time;

#endif
