/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_tcp.c — TCP 连接管理 + LoRa 帧收发
 *
 * TCP 连接/断开、数据接收与帧解析、ACK/数据帧发送。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lora_net.h"
#include "crc16.h"

/* ================================================================
 * TCP 内部状态 (不暴露到头文件)
 * ================================================================ */

static SOCKET  tcp_sock = INVALID_SOCKET;
static uint8_t tcp_rx_buf[RX_BUF_MAX];
static int     tcp_rx_len = 0;

/* ================================================================
 * 内部帧发送
 * ================================================================ */

static void send_ack(net_ctx_t *ctx, uint32_t nid)
{
    if (tcp_sock == INVALID_SOCKET) return;

    uint8_t buf[4 + 8];
    put_be32(buf, nid);
    int len = net_build_frame(buf + LORA_GATEWAY_PREFIX,
                              sizeof(buf) - LORA_GATEWAY_PREFIX,
                              nid, NULL, 0);
    if (len <= 0) return;

    send(tcp_sock, (const char *)buf, LORA_GATEWAY_PREFIX + len, 0);
    g_tx_count++;
    ctx->cb.log_hex(ctx->user_data, "TX ACK", buf, LORA_GATEWAY_PREFIX + len);
    ctx->cb.update_stats(ctx->user_data);
}

/* ================================================================
 * 公共发送 API
 * ================================================================ */

void net_send_ack(net_ctx_t *ctx, uint32_t nid)
{
    send_ack(ctx, nid);
}

void net_send_data_frame(net_ctx_t *ctx, uint32_t nid,
                         const uint8_t *data, uint16_t data_len)
{
    if (tcp_sock == INVALID_SOCKET) return;

    uint8_t buf[4 + 256];
    put_be32(buf, nid);
    int len = net_build_frame(buf + LORA_GATEWAY_PREFIX,
                              sizeof(buf) - LORA_GATEWAY_PREFIX,
                              nid, data, data_len);
    if (len <= 0) return;

    send(tcp_sock, (const char *)buf, LORA_GATEWAY_PREFIX + len, 0);
    g_tx_count++;
    ctx->cb.log_hex(ctx->user_data, "TX", buf, LORA_GATEWAY_PREFIX + len);
    ctx->cb.add_history_entry(ctx->user_data, nid, "TX", data, data_len);
    ctx->cb.update_stats(ctx->user_data);
}

/* ================================================================
 * 帧解析
 * ================================================================ */

static int parse_frame(net_ctx_t *ctx, const uint8_t *data, int len)
{
    if (len < LORA_FRAME_OVERHEAD) return 0;

    uint32_t nid = get_be32(data);
    uint16_t data_len = get_be16(data + LORA_FRAME_NID_SIZE);

    int total = LORA_FRAME_HEADER_SIZE + data_len + LORA_FRAME_CRC_SIZE;
    if (len < total) return 0;

    uint16_t calc_crc = crc16_ccitt(0, data, LORA_FRAME_HEADER_SIZE + data_len);
    uint16_t rx_crc = get_be16(data + LORA_FRAME_HEADER_SIZE + data_len);

    if (calc_crc != rx_crc) {
        g_err_count++;
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "CRC error! calc=%04X rx=%04X", calc_crc, rx_crc);
        ctx->cb.log_append(ctx->user_data, dbg);
        ctx->cb.log_hex(ctx->user_data, "RX (bad CRC)", data, total);
        ctx->cb.update_stats(ctx->user_data);
        return total;
    }

    if (g_nid != 0 && nid != g_nid) {
        ctx->cb.log_hex(ctx->user_data, "RX (NID mismatch)", data, total);
        return total;
    }

    /* 更新 NID 显示 */
    g_nid = nid;
    {
        char nid_buf[16];
        snprintf(nid_buf, sizeof(nid_buf), "%08X", nid);
        ctx->cb.set_nid_text(ctx->user_data, nid_buf);
    }

    g_rx_count++;
    const uint8_t *payload = data + LORA_FRAME_HEADER_SIZE;

    if (data_len == 0) {
        ctx->cb.log_append(ctx->user_data, "RX ACK (empty payload)");
        ctx->cb.add_history_entry(ctx->user_data, nid, "ACK", NULL, 0);
    } else if (data_len == 8 &&
               payload[5] == 0xFF && payload[6] == 0xFF && payload[7] == 0xFF) {
        g_last_x = (int16_t)get_be16(payload);
        g_last_y = (int16_t)get_be16(payload + 2);
        g_last_btn = payload[4] & 0x01;

        char desc[128];
        snprintf(desc, sizeof(desc), "Telemetry: X=%.1f Y=%.1f Btn=%s",
                 g_last_x / 10.0, g_last_y / 10.0,
                 g_last_btn ? "Released" : "Pressed");
        ctx->cb.log_append(ctx->user_data, desc);

        char detail[64];
        snprintf(detail, sizeof(detail), "X=%.1f Y=%.1f Btn=%d",
                 g_last_x / 10.0, g_last_y / 10.0, g_last_btn);
        ctx->cb.add_history_entry(ctx->user_data, nid, "Telemetry",
                                  (const uint8_t *)detail, (uint16_t)strlen(detail));

        ctx->cb.update_telemetry(ctx->user_data);

        if (g_auto_ack) {
            send_ack(ctx, nid);
        }
    } else if (data_len >= 2) {
        uint16_t can_id = get_be16(payload);
        char desc[256];
        int off = snprintf(desc, sizeof(desc), "Scanner CAN=0x%03X Data:", can_id);
        for (int i = 2; i < data_len && off < (int)sizeof(desc) - 4; i++) {
            off += snprintf(desc + off, sizeof(desc) - off, " %02X", payload[i]);
        }
        ctx->cb.log_append(ctx->user_data, desc);
        ctx->cb.add_history_entry(ctx->user_data, nid, "Scanner", payload, data_len);
    } else {
        ctx->cb.log_hex(ctx->user_data, "RX Data", data, total);
        ctx->cb.add_history_entry(ctx->user_data, nid, "Data", payload, data_len);
    }

    ctx->cb.log_hex(ctx->user_data, "RX", data, total);
    ctx->cb.update_stats(ctx->user_data);
    return total;
}

/* ================================================================
 * TCP 连接管理
 * ================================================================ */

void net_connect(net_ctx_t *ctx, const char *ip, int port)
{
    if (strlen(ip) == 0 || port <= 0 || port > 65535) {
        ctx->cb.show_error(ctx->user_data, "Error",
                           port <= 0 ? "Invalid port number" : "Please enter IP and Port");
        return;
    }

    tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp_sock == INVALID_SOCKET) {
        ctx->cb.show_error(ctx->user_data, "Error", "Failed to create socket");
        return;
    }

    u_long mode = 1;
    ioctlsocket(tcp_sock, FIONBIO, &mode);

    WSAAsyncSelect(tcp_sock, ctx->hwnd, WM_USER + 1,
                   FD_READ | FD_CLOSE | FD_CONNECT);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    ctx->cb.set_status_text(ctx->user_data, "  Connecting...");
    ctx->cb.set_connect_enabled(ctx->user_data, 0);

    int ret = connect(tcp_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(tcp_sock);
        tcp_sock = INVALID_SOCKET;
        ctx->cb.set_status_text(ctx->user_data, "  Connect failed");
        ctx->cb.set_connect_enabled(ctx->user_data, 1);
        ctx->cb.show_error(ctx->user_data, "Error", "Connect failed");
    }
}

void net_disconnect(net_ctx_t *ctx)
{
    if (tcp_sock != INVALID_SOCKET) {
        WSAAsyncSelect(tcp_sock, ctx->hwnd, 0, 0);
        closesocket(tcp_sock);
        tcp_sock = INVALID_SOCKET;
    }
    g_connected = FALSE;
    tcp_rx_len = 0;
    ctx->cb.update_connection_status(ctx->user_data);
    ctx->cb.log_append(ctx->user_data, "Disconnected");
}

void net_process_rx(net_ctx_t *ctx)
{
    char buf[2048];
    int bytes = recv(tcp_sock, buf, sizeof(buf), 0);
    if (bytes <= 0) return;

    if (tcp_rx_len + bytes > RX_BUF_MAX) {
        tcp_rx_len = 0;
    }
    memcpy(tcp_rx_buf + tcp_rx_len, buf, bytes);
    tcp_rx_len += bytes;

    while (tcp_rx_len >= LORA_FRAME_OVERHEAD) {
        int consumed = parse_frame(ctx, tcp_rx_buf, tcp_rx_len);
        if (consumed == 0) break;
        if (consumed > tcp_rx_len) consumed = tcp_rx_len;
        tcp_rx_len -= consumed;
        if (tcp_rx_len > 0) {
            memmove(tcp_rx_buf, tcp_rx_buf + consumed, tcp_rx_len);
        }
    }
}

int net_on_socket_event(net_ctx_t *ctx, int event, int error)
{
    switch (event) {
    case FD_CONNECT:
        if (error) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "Connect failed (error %d)", error);
            ctx->cb.log_append(ctx->user_data, buf);
            closesocket(tcp_sock);
            tcp_sock = INVALID_SOCKET;
            g_connected = FALSE;
        } else {
            g_connected = TRUE;
            tcp_rx_len = 0;
            ctx->cb.log_append(ctx->user_data, "Connected to gateway");
        }
        ctx->cb.update_connection_status(ctx->user_data);
        break;

    case FD_READ:
        net_process_rx(ctx);
        break;

    case FD_CLOSE:
        ctx->cb.log_append(ctx->user_data, "Connection closed by remote");
        net_disconnect(ctx);
        break;

    default:
        return 0;
    }
    return 1;
}
