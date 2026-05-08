/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk_tcp.c — TCP connection management + frame send/recv
 *
 * Non-blocking connect with watcher thread.
 * Recv thread parses frames and invokes callbacks directly.
 * No HWND/PostMessage dependency.
 */

#include "lora_sdk_internal.h"
#include "crc16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Frame parsing — called from recv thread
 * ================================================================ */

static int parse_frame(lora_sdk_t *sdk, const uint8_t *data, int len)
{
    if (len < SDK_FRAME_OVERHEAD) return 0;

    uint32_t nid = sdk_get_be32(data);
    uint16_t data_len = sdk_get_be16(data + SDK_FRAME_NID_SIZE);

    int total = SDK_FRAME_HEADER_SIZE + data_len + SDK_FRAME_CRC_SIZE;
    if (len < total) return 0;
    if (total > 2048) return -1;  /* 异常帧头，跳过 1 字节重同步 */

    uint16_t calc_crc = crc16_ccitt(0, data, SDK_FRAME_HEADER_SIZE + data_len);
    uint16_t rx_crc = sdk_get_be16(data + SDK_FRAME_HEADER_SIZE + data_len);

    if (calc_crc != rx_crc) {
        sdk->err_count++;
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "CRC error! calc=%04X rx=%04X", calc_crc, rx_crc);
        SDK_CALL(sdk, on_log, dbg);
        SDK_CALL(sdk, on_hex_dump, "RX (bad CRC)", data, total);
        return total;
    }

    if (sdk->nid != 0 && nid != sdk->nid) {
        SDK_CALL(sdk, on_hex_dump, "RX (NID mismatch)", data, total);
        return total;
    }

    sdk->nid = nid;
    sdk->rx_count++;

    const uint8_t *payload = data + SDK_FRAME_HEADER_SIZE;

    /* 空载荷: 不触发 on_frame */
    if (data_len == 0) {
        SDK_CALL(sdk, on_log, "RX (empty payload, ACK)");
        SDK_CALL(sdk, on_hex_dump, "RX", data, total);
        return total;
    }

    /* 触发 on_frame 回调 — payload 包含类型字节 */
    SDK_CALL(sdk, on_frame, nid, payload, data_len);
    SDK_CALL(sdk, on_hex_dump, "RX", data, total);
    return total;
}

/* ================================================================
 * TCP recv thread — select + recv + inline frame parsing
 * ================================================================ */

static DWORD WINAPI tcp_recv_worker(LPVOID param)
{
    lora_sdk_t *sdk = (lora_sdk_t *)param;

    while (InterlockedCompareExchange(&sdk->tcp_running, 1, 1)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sdk->tcp_sock, &fds);
        struct timeval tv = { 1, 0 };
        int sel = select(0, &fds, NULL, NULL, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel == 0) continue;

        uint8_t buf[2048];
        int bytes = recv(sdk->tcp_sock, (char *)buf, sizeof(buf), 0);
        if (bytes == 0 || bytes == SOCKET_ERROR) break;

        /* 缓冲区溢出保护 */
        if (sdk->tcp_rx_len + bytes > SDK_RX_BUF_MAX) {
            SDK_CALL(sdk, on_log, "RX buffer overflow, data dropped");
            sdk->tcp_rx_len = 0;
        }
        memcpy(sdk->tcp_rx_buf + sdk->tcp_rx_len, buf, bytes);
        sdk->tcp_rx_len += bytes;

        /* 帧解析循环 */
        while (sdk->tcp_rx_len > 0) {
            if (sdk->tcp_rx_buf[0] != SDK_FRAME_HDR1) {
                sdk->tcp_rx_len--;
                if (sdk->tcp_rx_len > 0)
                    memmove(sdk->tcp_rx_buf, sdk->tcp_rx_buf + 1, sdk->tcp_rx_len);
                continue;
            }
            if (sdk->tcp_rx_len < 2) break;
            if (sdk->tcp_rx_buf[1] != SDK_FRAME_HDR2) {
                sdk->tcp_rx_len--;
                if (sdk->tcp_rx_len > 0)
                    memmove(sdk->tcp_rx_buf, sdk->tcp_rx_buf + 1, sdk->tcp_rx_len);
                continue;
            }

            /* 查找帧尾 \r\n */
            int tail_pos = -1;
            for (int i = 2; i + 1 < sdk->tcp_rx_len; i++) {
                if (sdk->tcp_rx_buf[i] == 0x0D && sdk->tcp_rx_buf[i + 1] == 0x0A) {
                    tail_pos = i;
                    break;
                }
            }
            if (tail_pos < 0) break;

            int content_len = tail_pos - 2;
            int total_len = tail_pos + 2;

            if (content_len >= SDK_FRAME_OVERHEAD) {
                int consumed = parse_frame(sdk, sdk->tcp_rx_buf + 2, content_len);
                if (consumed <= 0) {
                    sdk->err_count++;
                    SDK_CALL(sdk, on_log, "Frame parse failed, re-syncing");
                }
            } else if (content_len > 0) {
                sdk->err_count++;
                SDK_CALL(sdk, on_log, "Frame content too short");
            }

            sdk->tcp_rx_len -= total_len;
            if (sdk->tcp_rx_len > 0)
                memmove(sdk->tcp_rx_buf, sdk->tcp_rx_buf + total_len, sdk->tcp_rx_len);
        }
    }

    /* 远端断开或错误退出 */
    if (InterlockedCompareExchange(&sdk->tcp_running, 1, 1)) {
        InterlockedExchange(&sdk->connected, 0);
        SDK_CALL(sdk, on_conn_state, LORA_SDK_CONN_DISCONNECTED);
    }
    return 0;
}

/* ================================================================
 * Connect watcher thread — 替代 WSAAsyncSelect
 * ================================================================ */

static DWORD WINAPI connect_watcher(LPVOID param)
{
    lora_sdk_t *sdk = (lora_sdk_t *)param;

    for (int i = 0; i < 20; i++) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sdk->tcp_sock, &write_fds);

        struct timeval tv = { 0, 500000 };
        int sel = select(0, NULL, &write_fds, NULL, &tv);

        if (sel == SOCKET_ERROR) {
            SDK_CALL(sdk, on_log, "Connect failed (select error)");
            closesocket(sdk->tcp_sock);
            sdk->tcp_sock = INVALID_SOCKET;
            InterlockedExchange(&sdk->connected, 0);
            SDK_CALL(sdk, on_conn_state, LORA_SDK_CONN_DISCONNECTED);
            return 0;
        }

        if (sel > 0 && FD_ISSET(sdk->tcp_sock, &write_fds)) {
            int err = 0, errlen = sizeof(err);
            getsockopt(sdk->tcp_sock, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);

            if (err == 0) {
                InterlockedExchange(&sdk->connected, 1);
                sdk->tcp_rx_len = 0;
                SDK_CALL(sdk, on_log, "Connected to gateway");
                SDK_CALL(sdk, on_conn_state, LORA_SDK_CONN_CONNECTED);

                InterlockedExchange(&sdk->tcp_running, 1);
                sdk->tcp_recv_thread = CreateThread(NULL, 0, tcp_recv_worker,
                                                     sdk, 0, NULL);
            } else {
                char buf[128];
                snprintf(buf, sizeof(buf), "Connect failed (error %d)", err);
                SDK_CALL(sdk, on_log, buf);
                closesocket(sdk->tcp_sock);
                sdk->tcp_sock = INVALID_SOCKET;
                InterlockedExchange(&sdk->connected, 0);
                SDK_CALL(sdk, on_conn_state, LORA_SDK_CONN_DISCONNECTED);
            }
            return 0;
        }
    }

    /* 超时 */
    SDK_CALL(sdk, on_log, "Connect failed (timeout 10s)");
    closesocket(sdk->tcp_sock);
    sdk->tcp_sock = INVALID_SOCKET;
    InterlockedExchange(&sdk->connected, 0);
    SDK_CALL(sdk, on_conn_state, LORA_SDK_CONN_DISCONNECTED);
    return 0;
}

/* ================================================================
 * Public TCP functions
 * ================================================================ */

void sdk_tcp_connect(lora_sdk_t *sdk, const char *ip, int port)
{
    sdk->tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sdk->tcp_sock == INVALID_SOCKET) {
        SDK_CALL(sdk, on_error, "Failed to create socket");
        return;
    }

    u_long mode = 1;
    ioctlsocket(sdk->tcp_sock, FIONBIO, &mode);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    SDK_CALL(sdk, on_conn_state, LORA_SDK_CONN_CONNECTING);

    int ret = connect(sdk->tcp_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(sdk->tcp_sock);
        sdk->tcp_sock = INVALID_SOCKET;
        SDK_CALL(sdk, on_error, "Connect failed");
        SDK_CALL(sdk, on_conn_state, LORA_SDK_CONN_DISCONNECTED);
        return;
    }

    sdk->tcp_connect_thread = CreateThread(NULL, 0, connect_watcher, sdk, 0, NULL);
}

static void stop_recv_thread(lora_sdk_t *sdk)
{
    if (!sdk->tcp_recv_thread) return;
    InterlockedExchange(&sdk->tcp_running, 0);
    if (sdk->tcp_sock != INVALID_SOCKET) {
        closesocket(sdk->tcp_sock);
        sdk->tcp_sock = INVALID_SOCKET;
    }
    WaitForSingleObject(sdk->tcp_recv_thread, 3000);
    CloseHandle(sdk->tcp_recv_thread);
    sdk->tcp_recv_thread = NULL;
}

void sdk_tcp_disconnect(lora_sdk_t *sdk)
{
    stop_recv_thread(sdk);
    InterlockedExchange(&sdk->connected, 0);
    sdk->tcp_rx_len = 0;
    SDK_CALL(sdk, on_conn_state, LORA_SDK_CONN_DISCONNECTED);
    SDK_CALL(sdk, on_log, "Disconnected");
}

void sdk_tcp_send_frame(lora_sdk_t *sdk, uint32_t nid,
                        const uint8_t *data, uint16_t data_len)
{
    if (sdk->tcp_sock == INVALID_SOCKET) return;

    uint8_t buf[SDK_GATEWAY_PREFIX + SDK_FRAME_WRAPPER + 256];
    int off = 0;

    /* NID 前缀 */
    sdk_put_be32(buf, nid);
    off += SDK_GATEWAY_PREFIX;

    /* 帧头 */
    buf[off++] = SDK_FRAME_HDR1;
    buf[off++] = SDK_FRAME_HDR2;

    int len = sdk_build_frame(buf + off, sizeof(buf) - off - 2, nid, data, data_len);
    if (len <= 0) return;
    off += len;

    /* 帧尾 */
    buf[off++] = '\r';
    buf[off++] = '\n';

    send(sdk->tcp_sock, (const char *)buf, off, 0);
    sdk->tx_count++;
    SDK_CALL(sdk, on_hex_dump, "TX", buf, off);
}

void sdk_tcp_send_rssi(lora_sdk_t *sdk, uint32_t nid,
                       uint8_t snr, uint8_t rssi, uint8_t test_flag)
{
    if (sdk->tcp_sock == INVALID_SOCKET) return;

    uint8_t payload[4] = { 0x03, snr, rssi, test_flag };
    sdk_tcp_send_frame(sdk, nid, payload, 3);

    char desc[64];
    snprintf(desc, sizeof(desc), "TX RSSI: raw=%d test=%d", (int)(int8_t)rssi, test_flag);
    SDK_CALL(sdk, on_log, desc);
}
