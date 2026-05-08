/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk_net.c — Frame protocol helpers
 *
 * Endianness conversion + unified frame building.
 * Frame format: [NID 4B BE][Length 2B BE][Data NB][CRC16 2B BE]
 */

#include "lora_sdk_internal.h"
#include "crc16.h"

void sdk_put_be32(uint8_t *buf, uint32_t val)
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8)  & 0xFF;
    buf[3] = val & 0xFF;
}

void sdk_put_be16(uint8_t *buf, uint16_t val)
{
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

uint32_t sdk_get_be32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  | buf[3];
}

uint16_t sdk_get_be16(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

int sdk_build_frame(uint8_t *out, size_t out_size, uint32_t nid,
                    const uint8_t *data, uint16_t data_len)
{
    int total = SDK_FRAME_HEADER_SIZE + data_len + SDK_FRAME_CRC_SIZE;
    if ((size_t)total > out_size) return -1;

    sdk_put_be32(out, nid);
    sdk_put_be16(out + 4, data_len);
    if (data_len > 0 && data)
        memcpy(out + SDK_FRAME_HEADER_SIZE, data, data_len);

    uint16_t crc = crc16_ccitt(0, out, SDK_FRAME_HEADER_SIZE + data_len);
    sdk_put_be16(out + SDK_FRAME_HEADER_SIZE + data_len, crc);
    return total;
}
