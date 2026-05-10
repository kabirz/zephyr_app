/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_net.h — LoRa Gateway Network Layer Interface
 *
 * TCP data streaming + UDP device configuration + frame protocol.
 * 数据帧格式: TX [NID 4B][0xAA][0x55][统一帧][\r\n], RX [0xAA][0x55][统一帧][\r\n].
 * Decoupled from Win32 GUI via callback registration.
 */

#ifndef LORA_NET_H
#define LORA_NET_H

#include <stdint.h>
#include <stddef.h>

/* ----------------------------------------------------------------
 * Constants — 与 lora.h 一致
 * ---------------------------------------------------------------- */
#define LORA_FRAME_NID_SIZE    4
#define LORA_FRAME_LEN_SIZE    2
#define LORA_FRAME_CRC_SIZE    2
#define LORA_FRAME_HEADER_SIZE (LORA_FRAME_NID_SIZE + LORA_FRAME_LEN_SIZE)
#define LORA_FRAME_OVERHEAD    (LORA_FRAME_HEADER_SIZE + LORA_FRAME_CRC_SIZE)
#define LORA_GATEWAY_PREFIX    4   /* 发送时在帧头额外添加的 NID 前缀 */

/* 数据帧封装: [0xAA][0x55][content][\r\n] */
#define LORA_FRAME_HDR_BYTE1   0xAA
#define LORA_FRAME_HDR_BYTE2   0x55
#define LORA_FRAME_WRAPPER_SIZE 4  /* 2 header + 2 footer (\r\n) */

#define RX_BUF_MAX    4096

/* ----------------------------------------------------------------
 * 数据类型 — 与 lora.h enum lora_data_type 一致
 * Data 字段首字节为类型标识
 * ---------------------------------------------------------------- */
enum lora_data_type {
    LORA_DATA_HANDLER = 0x01,   /* 手柄遥测 / CAN 扫描仪数据 */
    LORA_DATA_TEST    = 0x02,   /* 测试数据 */
    LORA_DATA_RSSI    = 0x03,   /* RSSI 信号强度请求/响应 */
};

/* ----------------------------------------------------------------
 * 扫描仪合并帧数据 (20 字节 payload)
 * ---------------------------------------------------------------- */
#define LORA_SCANNER_F_OVERBREAK  0x01
#define LORA_SCANNER_F_LASER      0x02
#define LORA_SCANNER_F_COORD_Z    0x04
#define LORA_SCANNER_F_COORD_XY   0x08

#define LORA_SCANNER_FRAME_SIZE   20

typedef struct {
    uint8_t  flags;
    int16_t  overbreak;
    uint32_t laser;
    int32_t  coord_x;
    int32_t  coord_y;
    int32_t  coord_z;
} scanner_data_t;

/* ----------------------------------------------------------------
 * 回调结构体 — UI 层填写并传入 net_init()
 * ---------------------------------------------------------------- */
typedef struct {
    /* 数据页回调 */
    void (*update_stats)(void *ud);
    void (*update_telemetry)(void *ud);
    void (*add_history_entry)(void *ud, uint32_t nid, const char *type,
                              const uint8_t *data, uint16_t data_len);
    void (*log_append)(void *ud, const char *text);
    void (*log_hex)(void *ud, const char *prefix,
                    const uint8_t *data, int len);
    void (*on_test_frame)(void *ud, uint32_t nid, uint16_t index, uint32_t timestamp);

    /* 配置页回调 */
    void (*cfg_log_append)(void *ud, const char *text);

    /* UI 控件更新 — SetWindowText / EnableWindow 等的抽象 */
    void (*set_nid_text)(void *ud, const char *nid_hex);
    void (*set_status_text)(void *ud, const char *text);
    void (*set_connect_enabled)(void *ud, int enabled);
    void (*set_disconnect_enabled)(void *ud, int enabled);
    void (*set_cfg_device_mac)(void *ud, const char *text);
    void (*set_cfg_device_name)(void *ud, const char *text);
    void (*set_cfg_device_sw)(void *ud, const char *text);
    void (*set_cfg_ip)(void *ud, const char *text);
    void (*set_cfg_sm)(void *ud, const char *text);
    void (*set_cfg_gw)(void *ud, const char *text);
    void (*set_cfg_gwid)(void *ud, const char *text);
    void (*set_cfg_csq)(void *ud, const char *text);
    void (*set_cfg_cmd_edit)(void *ud, const char *text);
    void (*set_cfg_dhcp)(void *ud, const char *text);
    void (*set_cfg_ip_edit)(void *ud, const char *text);
    void (*set_cfg_sm_edit)(void *ud, const char *text);
    void (*set_cfg_gw_edit)(void *ud, const char *text);
    void (*set_cfg_option)(void *ud, const char *text);
    void (*set_cfg_option_edit)(void *ud, const char *text);
    void (*set_cfg_nwmode)(void *ud, const char *text);
    void (*set_cfg_ttmode)(void *ud, const char *text);
    void (*set_cfg_wmode)(void *ud, const char *text);
    void (*set_cfg_upwid)(void *ud, const char *text);
    void (*set_cfg_ch)(void *ud, const char *text);
    void (*set_cfg_spd)(void *ud, const char *text);
    void (*set_cfg_pwr)(void *ud, const char *text);
    void (*show_error)(void *ud, const char *title, const char *message);
    void (*update_connection_status)(void *ud);
} net_callbacks_t;

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

/* 网络上下文 — 定义在头文件，供 lora_tcp.c / lora_udp.c 访问成员 */
typedef struct net_ctx {
    void             *hwnd;       /* HWND: 用于 WSAAsyncSelect / PostMessage */
    net_callbacks_t   cb;         /* 回调函数表 (拷贝) */
    void             *user_data;  /* 透传给回调 */
} net_ctx_t;

/* UDP 工作线程参数 */
typedef struct {
    int plen;
    uint8_t payload[2048];
    char target_ip[64];
    void *hwnd;                /* HWND, void* 避免头文件依赖 windows.h */
} udp_work_t;

/* TCP 接收线程传递给 UI 线程的数据块 */
typedef struct {
    int  len;
    uint8_t data[1];              /* 变长 */
} tcp_rx_chunk_t;

/* WM_UDP_RX 消息携带的数据块 */
typedef struct {
    char from_ip[64];
    char data[1];              /* 变长 */
} udp_rx_msg_t;

/* ----------------------------------------------------------------
 * 外部全局变量 — lora_net.c 定义，各模块可访问
 * ---------------------------------------------------------------- */

/* TCP 连接状态 (lora_tcp.c 定义和读写, UI 读) */
extern int  g_connected;

/* 协议状态 (跨模块共享) */
extern uint32_t g_nid;
extern uint32_t g_pending_rssi_nid;   /* RSSI 请求方 NID (TCP 写, UDP 读) */

/* 计数器 (lora_tcp.c 写, UI 读) */
extern int g_rx_count;
extern int g_tx_count;
extern int g_err_count;

/* 遥测数据 (lora_tcp.c 写, UI 读) */
extern int16_t  g_last_x;
extern int16_t  g_last_y;
extern uint8_t  g_last_btn;

/* UDP 设备信息 (lora_udp.c 读写) */
extern char g_dev_mac[32];
extern char g_dev_addr[64];
extern char g_dev_ip[64];
extern char g_dev_sm[64];
extern char g_dev_gw[64];
extern char g_dev_gwid[32];
extern char g_dev_name[64];
extern char g_dev_sw[32];
extern char g_local_if_ip[64];

/* ----------------------------------------------------------------
 * 生命周期 (lora_net.c)
 * ---------------------------------------------------------------- */

net_ctx_t *net_init(void *hwnd, const net_callbacks_t *callbacks,
                    void *user_data);
void net_cleanup(net_ctx_t *ctx);

/* ----------------------------------------------------------------
 * TCP 操作 (lora_tcp.c)
 * ---------------------------------------------------------------- */

void net_connect(net_ctx_t *ctx, const char *ip, int port);
void net_disconnect(net_ctx_t *ctx);
void net_process_rx(net_ctx_t *ctx);
void net_on_tcp_rx(net_ctx_t *ctx, tcp_rx_chunk_t *chunk);
int  net_on_socket_event(net_ctx_t *ctx, int event, int error);
void net_send_data_frame(net_ctx_t *ctx, uint32_t nid,
                         const uint8_t *data, uint16_t data_len);
void net_send_rssi_response(net_ctx_t *ctx, uint32_t nid, uint8_t snr_raw,
                            uint8_t rssi_raw, uint8_t test_flag);

/* ----------------------------------------------------------------
 * UDP 配置操作 (lora_udp.c)
 * ---------------------------------------------------------------- */

void net_cfg_search(net_ctx_t *ctx);
void net_cfg_get_net(net_ctx_t *ctx);
void net_cfg_send(net_ctx_t *ctx, const char *cmd);
void net_cfg_quick(net_ctx_t *ctx, const char *cmd);
void net_on_udp_log(net_ctx_t *ctx, const char *text);
void net_on_udp_rx(net_ctx_t *ctx, udp_rx_msg_t *msg);

/* ----------------------------------------------------------------
 * 帧协议辅助 (lora_net.c)
 * ---------------------------------------------------------------- */

int  net_build_frame(uint8_t *out, size_t out_size, uint32_t nid,
                     const uint8_t *data, uint16_t data_len);

void     put_be32(uint8_t *buf, uint32_t val);
void     put_be16(uint8_t *buf, uint16_t val);
uint32_t get_be32(const uint8_t *buf);
uint16_t get_be16(const uint8_t *buf);

/* Pack scanner struct into merged frame payload (20 bytes).
 * Returns 20 on success, -1 if buffer too small. */
static inline int scanner_pack(uint8_t *buf, size_t size,
                               const scanner_data_t *s)
{
    if (size < LORA_SCANNER_FRAME_SIZE) return -1;
    buf[0]  = LORA_DATA_HANDLER;
    buf[1]  = s->flags;
    put_be16(buf + 2, (uint16_t)s->overbreak);
    put_be32(buf + 4, s->laser);
    put_be32(buf + 8, (uint32_t)s->coord_x);
    put_be32(buf + 12, (uint32_t)s->coord_y);
    put_be32(buf + 16, (uint32_t)s->coord_z);
    return LORA_SCANNER_FRAME_SIZE;
}

#endif /* LORA_NET_H */
