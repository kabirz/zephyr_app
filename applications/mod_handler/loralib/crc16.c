/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CRC16-CCITT 实现 — 与 Zephyr crc16_ccitt(seed, src, len) 一致
 * 算法来源: Zephyr kernel/lib/os/crc16_sw.c
 */

#include "crc16.h"

uint16_t crc16_ccitt(uint16_t seed, const uint8_t *src, size_t len)
{
	for (; len > 0; len--) {
		uint8_t e, f;
		e = seed ^ *src;
		++src;
		f = e ^ (e << 4);
		seed = (seed >> 8) ^ ((uint16_t)f << 8) ^ ((uint16_t)f << 3) ^ ((uint16_t)f >> 4);
	}
	return seed;
}
