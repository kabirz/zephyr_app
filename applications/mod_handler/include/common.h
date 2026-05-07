
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
	uint8_t power_mv;
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

	/* 测试模式状态 (由 lora.c 读写, UI/Shell 读取) */
	bool test_mode;            /* true = 测试模式激活 */
	int8_t test_rssi_raw;      /* 网关测量的 RSSI (dBm) */
	int8_t test_snr_raw;       /* 网关测量的 SNR (dB) */
	uint32_t test_tx_count;    /* 测试包发送计数 */
	uint32_t test_rx_count;    /* 测试包接收计数 */
	uint32_t test_rtt_last;    /* 最近 RTT (ms) */
	uint32_t test_rtt_min;     /* 最小 RTT (ms), UINT32_MAX = 未初始化 */
	uint32_t test_rtt_max;     /* 最大 RTT (ms) */
	uint64_t test_rtt_sum;     /* RTT 累加和 (ms), 用于算平均 */
	uint16_t test_last_rx_idx; /* 上次收包序号 (丢包检测) */
	uint32_t test_gap_lost;    /* index 间隔检测到的丢包数 */
} gloval_params_t;

extern gloval_params_t global_params;
extern volatile uint32_t last_activity_time;

#endif
