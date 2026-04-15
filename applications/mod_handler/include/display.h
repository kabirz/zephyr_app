#ifndef _MOD_DISPLAY_H
#define _MOD_DISPLAY_H
#include <common.h>

typedef struct {
	char val;
	uint8_t data[16];
} font8x16_t;

int mod_display_init(void);
void mod_display_clear(void);
void mod_display_battery(const gloval_params_t *params);
void mod_display_handler_xy(const gloval_params_t *params);
void mod_display_handler_button(const gloval_params_t *params);
void mod_display_lora_can(const gloval_params_t *params);
void mod_display_scanner(const gloval_params_t *params);
void mod_display_all(const gloval_params_t *params);

#endif
