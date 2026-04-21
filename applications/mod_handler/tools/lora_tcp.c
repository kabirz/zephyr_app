/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_tcp.c — TCP 连接管理 + LoRa 帧收发
 *
 * TCP 连接/断开、数据接收与帧解析、ACK/数据帧发送。
 * 数据帧格式: [Gateway Prefix 4B][0xAA][0x55][统一帧][\r\n]
 * 接收使用独立线程，避免 WSAAsyncSelect FD_READ 的消息丢失问题。
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
 * TCP 内部状态
 * ================================================================ */

static SOCKET  tcp_sock = INVALID_SOCKET;
static HANDLE  tcp_recv_thread = NULL;
static volatile LONG tcp_running = 0;

static uint8_t tcp_rx_buf[RX_BUF_MAX];
static int     tcp_rx_len = 0;

/* UI 消息 ID — 与 lora_gateway_tool.c 定义一致 */
#define WM_TCP_RX      (WM_USER + 4)
#define WM_TCP_CLOSED  (WM_USER + 5)

/* ================================================================
 * 内部帧发送
 * ================================================================ */

static void send_ack(net_ctx_t *ctx, uint32_t nid)
{
    if (tcp_sock == INVALID_SOCKET) return;

    uint8_t ack_type = LORA_DATA_ACK;
    uint8_t buf[LORA_GATEWAY_PREFIX + LORA_FRAME_WRAPPER_SIZE + LORA_FRAME_OVERHEAD + 1];
    int off = 0;

    /* 网关定向前缀 */
    put_be32(buf, nid);
    off += LORA_GATEWAY_PREFIX;

    /* 帧头 */
    buf[off++] = LORA_FRAME_HDR_BYTE1;
    buf[off++] = LORA_FRAME_HDR_BYTE2;

    int len = net_build_frame(buf + off, sizeof(buf) - off - 2,
                              nid, &ack_type, 1);
    if (len <= 0) return;
    off += len;

    /* 帧尾 */
    buf[off++] = '\r';
    buf[off++] = '\n';

    send(tcp_sock, (const char *)buf, off, 0);
    g_tx_count++;
    ctx->cb.log_hex(ctx->user_data, "TX ACK", buf, off);
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

    uint8_t buf[LORA_GATEWAY_PREFIX + LORA_FRAME_WRAPPER_SIZE + 256];
    int off = 0;

    /* 网关定向前缀 */
    put_be32(buf, nid);
    off += LORA_GATEWAY_PREFIX;

    /* 帧头 */
    buf[off++] = LORA_FRAME_HDR_BYTE1;
    buf[off++] = LORA_FRAME_HDR_BYTE2;

    int len = net_build_frame(buf + off, sizeof(buf) - off - 2,
                              nid, data, data_len);
    if (len <= 0) return;
    off += len;

    /* 帧尾 */
    buf[off++] = '\r';
    buf[off++] = '\n';

    send(tcp_sock, (const char *)buf, off, 0);
    g_tx_count++;
    g_ack_pending = 1;
    ctx->cb.log_hex(ctx->user_data, "TX", buf, off);
    ctx->cb.add_history_entry(ctx->user_data, nid, "TX", data, data_len);
    ctx->cb.update_stats(ctx->user_data);
}

void net_send_rssi_response(net_ctx_t *ctx, uint32_t nid, uint8_t rssi)
{
    if (tcp_sock == INVALID_SOCKET) return;

    uint8_t payload[2] = { LORA_DATA_RSSI, rssi };
    uint8_t buf[LORA_GATEWAY_PREFIX + LORA_FRAME_WRAPPER_SIZE + LORA_FRAME_OVERHEAD + 2];
    int off = 0;

    /* 网关定向前缀 */
    put_be32(buf, nid);
    off += LORA_GATEWAY_PREFIX;

    /* 帧头 */
    buf[off++] = LORA_FRAME_HDR_BYTE1;
    buf[off++] = LORA_FRAME_HDR_BYTE2;

    int len = net_build_frame(buf + off, sizeof(buf) - off - 2,
                              nid, payload, 2);
    if (len <= 0) return;
    off += len;

    /* 帧尾 */
    buf[off++] = '\r';
    buf[off++] = '\n';

    send(tcp_sock, (const char *)buf, off, 0);
    g_tx_count++;
    char desc[64];
    snprintf(desc, sizeof(desc), "TX RSSI response: level %d", rssi);
    ctx->cb.log_append(ctx->user_data, desc);
    ctx->cb.log_hex(ctx->user_data, "TX RSSI", buf, off);
    ctx->cb.add_history_entry(ctx->user_data, nid, "TX RSSI", payload, 2);
    ctx->cb.update_stats(ctx->user_data);
}

/* ================================================================
 * 帧解析 — 在 UI 线程中调用
 * ================================================================ */

static int parse_frame(net_ctx_t *ctx, const uint8_t *data, int len)
{
    if (len < LORA_FRAME_OVERHEAD) return 0;

    uint32_t nid = get_be32(data);
    uint16_t data_len = get_be16(data + LORA_FRAME_NID_SIZE);

    int total = LORA_FRAME_HEADER_SIZE + data_len + LORA_FRAME_CRC_SIZE;
    if (len < total) return 0;

    /* data_len 过大说明帧头数据异常，跳过 1 字节重新同步 */
    if (total > 2048) return -1;

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

    /* 无载荷或载荷不足 1 字节: 跳过类型解析 */
    if (data_len == 0) {
        ctx->cb.log_append(ctx->user_data, "RX (empty payload, ignored)");
        ctx->cb.add_history_entry(ctx->user_data, nid, "Empty", NULL, 0);
        ctx->cb.log_hex(ctx->user_data, "RX", data, total);
        ctx->cb.update_stats(ctx->user_data);
        return total;
    }

    /* Data 首字节为类型标识 */
    uint8_t type = payload[0];
    const uint8_t *body = payload + 1;
    uint16_t body_len = data_len - 1;

    switch (type) {

    case LORA_DATA_ACK:
        ctx->cb.log_append(ctx->user_data, "RX ACK");
        ctx->cb.add_history_entry(ctx->user_data, nid, "ACK", NULL, 0);
        if (g_ack_pending) {
            g_ack_pending = 0;
            ctx->cb.log_append(ctx->user_data, "  Send confirmed");
        }
        break;

    case LORA_DATA_HANDLER:
        /* 遥测数据: 只接收方向 (手柄→网关→工具)
         * 扫描仪数据由工具端发出, 不会在此收到 */
        if (body_len == 8 &&
            body[5] == 0xFF && body[6] == 0xFF && body[7] == 0xFF) {
            g_last_x = (int16_t)get_be16(body);
            g_last_y = (int16_t)get_be16(body + 2);
            g_last_btn = body[4] & 0x01;

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
        } else {
            /* 非标准遥测格式: 记录原始数据 */
            char desc[64];
            snprintf(desc, sizeof(desc), "RX HANDLER (unexpected %d bytes)", body_len);
            ctx->cb.log_append(ctx->user_data, desc);
            ctx->cb.log_hex(ctx->user_data, "RX HANDLER data", body, body_len);
            ctx->cb.add_history_entry(ctx->user_data, nid, "Handler", body, body_len);
        }

        if (g_auto_ack) send_ack(ctx, nid);
        break;

    case LORA_DATA_TEST:
        ctx->cb.log_append(ctx->user_data, "RX TEST");
        ctx->cb.log_hex(ctx->user_data, "RX TEST data", body, body_len);
        ctx->cb.add_history_entry(ctx->user_data, nid, "Test", body, body_len);
        if (g_auto_ack) send_ack(ctx, nid);
        break;

    case LORA_DATA_RSSI:
        g_pending_rssi_nid = nid;
        ctx->cb.log_append(ctx->user_data, "RX RSSI request -> querying NINFO");
        ctx->cb.add_history_entry(ctx->user_data, nid, "RX RSSI", body, body_len);
        net_cfg_send(ctx, "AT+NINFO?\r\n");
        break;

    default: {
        char desc[64];
        snprintf(desc, sizeof(desc), "RX unknown type 0x%02X", type);
        ctx->cb.log_append(ctx->user_data, desc);
        ctx->cb.log_hex(ctx->user_data, "RX Data", data, total);
        ctx->cb.add_history_entry(ctx->user_data, nid, "Data", payload, data_len);
        break;
    }
    }

    ctx->cb.log_hex(ctx->user_data, "RX", data, total);
    ctx->cb.update_stats(ctx->user_data);
    return total;
}

/* ================================================================
 * TCP 接收线程 — select + recv 循环，PostMessage 到 UI 线程
 * ================================================================ */

static DWORD WINAPI tcp_recv_worker(LPVOID param)
{
    net_ctx_t *ctx = (net_ctx_t *)param;

    while (InterlockedCompareExchange(&tcp_running, 1, 1)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(tcp_sock, &fds);
        struct timeval tv = { 1, 0 };
        int sel = select(0, &fds, NULL, NULL, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel == 0) continue;

        uint8_t buf[2048];
        int bytes = recv(tcp_sock, (char *)buf, sizeof(buf), 0);
        if (bytes == 0 || bytes == SOCKET_ERROR) break;

        tcp_rx_chunk_t *chunk = (tcp_rx_chunk_t *)malloc(sizeof(tcp_rx_chunk_t) + bytes);
        if (!chunk) continue;
        chunk->len = bytes;
        memcpy(chunk->data, buf, bytes);
        PostMessage((HWND)ctx->hwnd, WM_TCP_RX, 0, (LPARAM)chunk);
    }

    if (InterlockedCompareExchange(&tcp_running, 1, 1)) {
        PostMessage((HWND)ctx->hwnd, WM_TCP_CLOSED, 0, 0);
    }
    return 0;
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
                   FD_CONNECT | FD_CLOSE);

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

static void stop_recv_thread(void)
{
    if (!tcp_recv_thread) return;
    InterlockedExchange(&tcp_running, 0);
    if (tcp_sock != INVALID_SOCKET) {
        /* closesocket 会解除 recv/select 阻塞 */
        closesocket(tcp_sock);
        tcp_sock = INVALID_SOCKET;
    }
    WaitForSingleObject(tcp_recv_thread, 3000);
    CloseHandle(tcp_recv_thread);
    tcp_recv_thread = NULL;
}

void net_disconnect(net_ctx_t *ctx)
{
    stop_recv_thread();
    g_connected = FALSE;
    tcp_rx_len = 0;
    ctx->cb.update_connection_status(ctx->user_data);
    ctx->cb.log_append(ctx->user_data, "Disconnected");
}

/* UI 线程处理接收到的数据块 */
void net_on_tcp_rx(net_ctx_t *ctx, tcp_rx_chunk_t *chunk)
{
    if (!chunk) return;
    int bytes = chunk->len;
    const uint8_t *data = chunk->data;

    if (tcp_rx_len + bytes > RX_BUF_MAX) {
        ctx->cb.log_append(ctx->user_data, "RX buffer overflow, data dropped");
        tcp_rx_len = 0;
    }
    memcpy(tcp_rx_buf + tcp_rx_len, data, bytes);
    tcp_rx_len += bytes;
    free(chunk);

    while (tcp_rx_len > 0) {
        /* 查找帧头 0xAA 0x55 */
        if (tcp_rx_buf[0] != LORA_FRAME_HDR_BYTE1) {
            tcp_rx_len--;
            if (tcp_rx_len > 0)
                memmove(tcp_rx_buf, tcp_rx_buf + 1, tcp_rx_len);
            continue;
        }
        if (tcp_rx_len < 2) break;
        if (tcp_rx_buf[1] != LORA_FRAME_HDR_BYTE2) {
            tcp_rx_len--;
            if (tcp_rx_len > 0)
                memmove(tcp_rx_buf, tcp_rx_buf + 1, tcp_rx_len);
            continue;
        }

        /* 查找帧尾 \r\n */
        int tail_pos = -1;
        for (int i = 2; i + 1 < tcp_rx_len; i++) {
            if (tcp_rx_buf[i] == 0x0D && tcp_rx_buf[i + 1] == 0x0A) {
                tail_pos = i;
                break;
            }
        }
        if (tail_pos < 0) break; /* 等待更多数据 */

        /* 剥离帧头(2) + 帧尾(2), 提交内容给 parse_frame */
        int content_len = tail_pos - 2;
        int total_len = tail_pos + 2;

        if (content_len >= LORA_FRAME_OVERHEAD) {
            int consumed = parse_frame(ctx, tcp_rx_buf + 2, content_len);
            if (consumed <= 0) {
                g_err_count++;
                ctx->cb.log_append(ctx->user_data,
                                   "Frame parse failed, re-syncing");
            }
        } else if (content_len > 0) {
            g_err_count++;
            ctx->cb.log_append(ctx->user_data, "Frame content too short");
        }

        tcp_rx_len -= total_len;
        if (tcp_rx_len > 0) {
            memmove(tcp_rx_buf, tcp_rx_buf + total_len, tcp_rx_len);
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
            /* 连接成功：禁用 WSAAsyncSelect，启动接收线程 */
            WSAAsyncSelect(tcp_sock, ctx->hwnd, 0, 0);
            g_connected = TRUE;
            tcp_rx_len = 0;
            ctx->cb.log_append(ctx->user_data, "Connected to gateway");

            InterlockedExchange(&tcp_running, 1);
            tcp_recv_thread = CreateThread(NULL, 0, tcp_recv_worker, ctx, 0, NULL);
        }
        ctx->cb.update_connection_status(ctx->user_data);
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
