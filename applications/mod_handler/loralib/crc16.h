/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CRC16-CCITT (多項式 0x1021, 初始值 0)
 * 与 Zephyr crc16_ccitt() 一致，用于 LoRa 统一帧校验
 */

#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, size_t len);

#endif /* CRC16_H */
