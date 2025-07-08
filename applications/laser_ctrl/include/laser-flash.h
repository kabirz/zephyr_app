#ifndef __LASER__FLASH_H__
#define __LASER__FLASH_H__

#include <stdint.h>

int laser_flash_write(uint8_t address, uint32_t val);
int laser_flash_read(uint8_t address, uint32_t *val);

#endif
