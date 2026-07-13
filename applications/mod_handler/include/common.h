
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
#define RF24_EVENT     BIT(3) /* 2.4G (nRF24L01+) 无线模式事件 */

typedef struct {
	int x_degree;
	int y_degree;
	uint8_t h_button;
	uint32_t power_mv;
#define CAN_TYPE   1
#define RF24_TYPE  2 /* 2.4G (nRF24L01+) 无线模式 */
#define REPORT_PERIOD_MS 800   /* 周期状态上报间隔，CAN/RF24 共用 */
	uint8_t connect_type;
	uint32_t report_period;
	battery_status_t battery_status;
	scanner_data_t scanner;
	uint8_t rssi;
	volatile bool sleeping;
	struct k_event event;
	bool log;
} global_params_t;

extern global_params_t global_params;

/* 按 connect_type 分派发送手柄状态帧 (CAN 0x1E3 / RF24 遥测)。返回 0 成功，<0 失败。*/
int send_handler_state(const global_params_t *params);

extern volatile uint32_t last_activity_time;

#endif
