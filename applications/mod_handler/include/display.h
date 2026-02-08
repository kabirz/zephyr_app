#ifndef _MOD_DISPLAY_H
#define _MOD_DISPLAY_H
#include <common.h>

void mod_display_init(void);
void mod_display_battery(const gloval_params_t *params);
void mod_display_handler_x(const gloval_params_t *params);
void mod_display_handler_y(const gloval_params_t *params);
void mod_display_handler_button(const gloval_params_t *params);
void mod_display_lora_can(const gloval_params_t *params);
void mod_display_all(const gloval_params_t *params);

#endif
