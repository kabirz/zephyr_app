/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk_internal.h — SDK internal types and function declarations
 *
 * Shared between lora_sdk.c, lora_sdk_tcp.c, lora_sdk_udp.c, lora_sdk_net.c.
 * NOT part of the public API — do not distribute to customers.
 */

#ifndef LORA_SDK_INTERNAL_H
#define LORA_SDK_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "lora_sdk.h"

/* ================================================================
 * Frame constants — 与嵌入式端 lora.h 一致
 * ================================================================ */

#define SDK_FRAME_NID_SIZE    4
#define SDK_FRAME_LEN_SIZE    2
#define SDK_FRAME_CRC_SIZE    2
#define SDK_FRAME_HEADER_SIZE (SDK_FRAME_NID_SIZE + SDK_FRAME_LEN_SIZE)
#define SDK_FRAME_OVERHEAD    (SDK_FRAME_HEADER_SIZE + SDK_FRAME_CRC_SIZE)

/* 数据帧封装: [0xAA][0x55][content][\r\n] */
#define SDK_FRAME_HDR1        0xAA
#define SDK_FRAME_HDR2        0x55
#define SDK_FRAME_WRAPPER     4   /* 2 header + 2 footer */

/* 发送时额外添加的 NID 前缀 */
#define SDK_GATEWAY_PREFIX    4

#define SDK_RX_BUF_MAX        4096
#define SDK_UDP_MAX_IFACES    16

/* ================================================================
 * SDK 实例结构 — 封装所有状态（实例化，非全局）
 * ================================================================ */

struct lora_sdk {
    /* 回调 */
    lora_sdk_callbacks_t  cbs;
    void                 *user_data;

    /* TCP 状态 (sdk_tcp_* 函数读写) */
    SOCKET           tcp_sock;
    HANDLE           tcp_recv_thread;
    HANDLE           tcp_connect_thread;
    volatile LONG    tcp_running;
    uint8_t          tcp_rx_buf[SDK_RX_BUF_MAX];
    int              tcp_rx_len;

    /* 连接状态 */
    volatile LONG    connected;   /* 0=disconnected, 1=connected */

    /* 协议状态 */
    uint32_t         nid;
    uint32_t         pending_rssi_nid;
    int              test_flag;

    /* 计数器 */
    int              rx_count;
    int              tx_count;
    int              err_count;

    /* 遥测 (最近收到的 HANDLER 数据) */
    int16_t          last_x;
    int16_t          last_y;
    uint8_t          last_btn;

    /* UDP 设备信息 (sdk_udp_* 函数读写) */
    char             dev_mac[32];
    char             dev_addr[64];
    char             dev_ip[64];
    char             dev_sm[64];
    char             dev_gw[64];
    char             dev_gwid[32];
    char             dev_name[64];
    char             dev_sw[32];
    char             local_if_ip[64];
};

/* ================================================================
 * Callback helper — 空安全调用
 * ================================================================ */

#define SDK_CALL(sdk, fn, ...) \
    do { if ((sdk)->cbs.fn) (sdk)->cbs.fn((sdk)->user_data, __VA_ARGS__); } while (0)

/* ================================================================
 * Internal function declarations
 * ================================================================ */

/* lora_sdk_net.c */
void     sdk_put_be32(uint8_t *buf, uint32_t val);
void     sdk_put_be16(uint8_t *buf, uint16_t val);
uint32_t sdk_get_be32(const uint8_t *buf);
uint16_t sdk_get_be16(const uint8_t *buf);
int      sdk_build_frame(uint8_t *out, size_t out_size, uint32_t nid,
                         const uint8_t *data, uint16_t data_len);

/* lora_sdk_tcp.c */
void sdk_tcp_connect(lora_sdk_t *sdk, const char *ip, int port);
void sdk_tcp_disconnect(lora_sdk_t *sdk);
void sdk_tcp_send_frame(lora_sdk_t *sdk, uint32_t nid,
                        const uint8_t *data, uint16_t data_len);
void sdk_tcp_send_rssi(lora_sdk_t *sdk, uint32_t nid,
                       uint8_t snr, uint8_t rssi, uint8_t test_flag);

/* lora_sdk_udp.c */
void sdk_udp_search(lora_sdk_t *sdk);
void sdk_udp_get_net(lora_sdk_t *sdk);
void sdk_udp_send_at(lora_sdk_t *sdk, const char *cmd);

#endif /* LORA_SDK_INTERNAL_H */
