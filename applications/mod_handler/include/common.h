
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
	float x_degree;
	float y_degree;
	uint8_t h_button;
	uint8_t power_level;
#define CAN_TYPE  1
#define LORA_TYPE 1
	uint8_t connect_type;
	uint32_t can_heart_time;
	battery_status_t battery_status;
#define TIMEOUT_EVENT 0x1
	struct k_event event;
} gloval_params_t;

extern gloval_params_t global_params;

#endif
