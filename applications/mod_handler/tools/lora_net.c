/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_net.c — LoRa Gateway 共享网络层
 *
 * 全局状态定义、endianness helpers、帧协议、生命周期管理。
 * 数据帧封装: [0xAA][0x55][content][\r\n], TCP 操作见 lora_tcp.c，UDP 操作见 lora_udp.c。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lora_net.h"
#include "crc16.h"

#pragma comment(lib, "ws2_32.lib")

/* ================================================================
 * 全局状态定义 (extern 声明在 lora_net.h)
 * ================================================================ */

/* TCP 状态 (g_sock/g_rx_buf/g_rx_len 在 lora_tcp.c 内部定义) */
int    g_connected = 0;

/* 协议状态 */
uint32_t g_nid = 0;
uint32_t g_pending_rssi_nid = 0;

/* 计数器 */
int g_rx_count = 0;
int g_tx_count = 0;
int g_err_count = 0;

/* 遥测 */
int16_t  g_last_x = 0;
int16_t  g_last_y = 0;
uint8_t  g_last_btn = 1;

/* UDP 设备信息 */
char g_dev_mac[32] = "";
char g_dev_addr[64] = "";
char g_dev_ip[64] = "";
char g_dev_sm[64] = "";
char g_dev_gw[64] = "";
char g_dev_gwid[32] = "";
char g_dev_name[64] = "";
char g_dev_sw[32] = "";

/* ================================================================
 * Endianness helpers
 * ================================================================ */

void put_be32(uint8_t *buf, uint32_t val)
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

void put_be16(uint8_t *buf, uint16_t val)
{
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

uint32_t get_be32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | buf[3];
}

uint16_t get_be16(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

/* ================================================================
 * 帧协议 — 组帧
 * ================================================================ */

int net_build_frame(uint8_t *out, size_t out_size, uint32_t nid,
                    const uint8_t *data, uint16_t data_len)
{
    int total = LORA_FRAME_HEADER_SIZE + data_len + LORA_FRAME_CRC_SIZE;
    if ((size_t)total > out_size) return -1;

    put_be32(out, nid);
    put_be16(out + 4, data_len);
    if (data_len > 0 && data)
        memcpy(out + LORA_FRAME_HEADER_SIZE, data, data_len);

    uint16_t crc = crc16_ccitt(0, out, LORA_FRAME_HEADER_SIZE + data_len);
    put_be16(out + LORA_FRAME_HEADER_SIZE + data_len, crc);
    return total;
}

/* ================================================================
 * 生命周期
 * ================================================================ */

net_ctx_t *net_init(void *hwnd, const net_callbacks_t *callbacks,
                    void *user_data)
{
    net_ctx_t *ctx = (net_ctx_t *)calloc(1, sizeof(net_ctx_t));
    if (!ctx) return NULL;

    ctx->hwnd = hwnd;
    ctx->cb = *callbacks;
    ctx->user_data = user_data;

    return ctx;
}

void net_cleanup(net_ctx_t *ctx)
{
    if (ctx) {
        net_disconnect(ctx);
        free(ctx);
    }
}
