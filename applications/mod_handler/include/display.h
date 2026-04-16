#ifndef _MOD_DISPLAY_H
#define _MOD_DISPLAY_H
#include <common.h>

typedef struct {
	char val;
	uint8_t data[16];
} font8x16_t;

int mod_display_init(void);
void mod_display_clear(void);
void mod_display_loar_rssi(uint8_t rssi);
void mod_display_battery(uint8_t power_level);
void mod_display_lora_can(uint8_t connect_type);
void mod_display_lora_gwid(uint32_t gwid);

void mod_display_scanner(const scanner_data_t *s);
void mod_display_handler_xy(int x, int y);
void mod_display_handler_button(uint8_t h_button);
void mod_display_all(const gloval_params_t *params);

#endif
