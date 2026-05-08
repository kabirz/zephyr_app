#ifndef _MOD_DISPLAY_H
#define _MOD_DISPLAY_H
#include <common.h>

void mod_display_reinit(void);
int mod_display_init(void);
void mod_display_clear(void);
void mod_display_lora(uint8_t rssi);
void mod_display_battery(uint32_t mv, battery_status_t status);
void mod_display_can(void);

void mod_display_scanner(const scanner_data_t *s);
void mod_display_handler_xy(int x, int y);
void mod_display_handler_button(uint8_t h_button);
void mod_display_all(const gloval_params_t *params);

/* 测试模式显示 (Row 1-3) */
void mod_display_test_rssi(int8_t rssi_raw, int8_t snr_raw);
void mod_display_test_loss(uint32_t loss_count);
void mod_display_test_rtt(uint32_t rtt_ms, uint32_t avg_ms);
void mod_display_test_all(const gloval_params_t *params);
void mod_display_normal_rows(const gloval_params_t *params);

#endif
