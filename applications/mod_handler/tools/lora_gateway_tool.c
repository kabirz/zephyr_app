/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoRa Gateway Host Tool — Win32 GUI + TCP Client + UDP Config
 *
 * 连接 USR-LG210-L 网关，接收/解析 LoRa 数据帧，发送 ACK。
 * 支持遥测数据显示、原始数据日志、手动发送、历史记录。
 * 通过 UDP 广播 AT 指令配置 LoRa 模块。
 *
 * 协议: [NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE]
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc16.h"
#include "cJSON.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "iphlpapi.lib")

/* ================================================================
 * 常量定义 — 与 lora.h 一致
 * ================================================================ */
#define LORA_FRAME_NID_SIZE    4
#define LORA_FRAME_LEN_SIZE    2
#define LORA_FRAME_CRC_SIZE    2
#define LORA_FRAME_HEADER_SIZE (LORA_FRAME_NID_SIZE + LORA_FRAME_LEN_SIZE)
#define LORA_FRAME_OVERHEAD    (LORA_FRAME_HEADER_SIZE + LORA_FRAME_CRC_SIZE)
#define LORA_GATEWAY_PREFIX    4   /* 发送时在帧头额外添加的定向 NID */

#define RX_BUF_MAX    4096
#define LOG_BUF_MAX   65536
#define HISTORY_MAX   500

#define WM_SOCKET       (WM_USER + 1)
#define WM_UDP_LOG      (WM_USER + 2)   /* LPARAM = strdup'd string, 接收后 free() */
#define WM_UDP_RX       (WM_USER + 3)   /* LPARAM = strdup'd raw response, 接收后 free() */

#define CLIENT_W      920
#define CLIENT_H      640

/* 数据 Tab 控件 ID */
#define IDC_TAB_CTRL        1100
#define IDC_IP_EDIT         1001
#define IDC_PORT_EDIT       1002
#define IDC_CONNECT_BTN     1003
#define IDC_DISCONNECT_BTN  1004
#define IDC_NID_EDIT        1005
#define IDC_SEND_EDIT       1007
#define IDC_SEND_BTN        1008
#define IDC_SEND_ACK_BTN    1009
#define IDC_CLEAR_LOG_BTN   1010
#define IDC_AUTO_ACK_CHECK  1011
#define IDC_STATUS_TEXT     1012
#define IDC_X_TEXT          1014
#define IDC_Y_TEXT          1015
#define IDC_BTN_TEXT        1016
#define IDC_RX_COUNT        1017
#define IDC_TX_COUNT        1018
#define IDC_ERR_COUNT       1019
#define IDC_LOG_EDIT        1020
#define IDC_HISTORY_LIST    1021

/* 配置 Tab 控件 ID */
#define IDC_CFG_CMD_EDIT    1200
#define IDC_CFG_SEND_BTN    1201
#define IDC_CFG_LOG_EDIT    1202
#define IDC_CFG_CLEAR_BTN   1203
#define IDC_CFG_QUERY_VER   1204
#define IDC_CFG_QUERY_CFG   1205
#define IDC_CFG_QUERY_GWID  1207
#define IDC_CFG_SEARCH_BTN  1211
#define IDC_CFG_GETNET_BTN  1212
#define IDC_CFG_QUERY_CSQ   1213

/* ================================================================
 * 全局状态
 * ================================================================ */
static HINSTANCE g_hInst;
static HWND g_hwndMain;

/* TCP */
static SOCKET g_sock = INVALID_SOCKET;
static BOOL g_connected = FALSE;
static uint8_t g_rx_buf[RX_BUF_MAX];
static int g_rx_len = 0;

static uint32_t g_nid = 0;
static BOOL g_auto_ack = TRUE;

static int g_rx_count = 0;
static int g_tx_count = 0;
static int g_err_count = 0;

static int16_t g_last_x = 0;
static int16_t g_last_y = 0;
static uint8_t g_last_btn = 1;

/* Tab 控件 */
static HWND g_hTabCtrl;

/* 数据页控件 */
static HWND g_hIpEdit, g_hPortEdit, g_hConnectBtn, g_hDisconnectBtn;
static HWND g_hNidEdit;
static HWND g_hSendEdit, g_hSendBtn, g_hSendAckBtn, g_hClearLogBtn;
static HWND g_hAutoAckCheck, g_hStatusText;
static HWND g_hXText, g_hYText, g_hBtnText;
static HWND g_hRxCount, g_hTxCount, g_hErrCount;
static HWND g_hLogEdit;
static HWND g_hHistoryList;

/* 配置页控件 */
static HWND g_hCfgCmdEdit, g_hCfgSendBtn, g_hCfgLogEdit, g_hCfgClearBtn;
static HWND g_hCfgQueryVer, g_hCfgQueryGwid, g_hCfgQueryCsq;
static HWND g_hCfgSearchBtn, g_hCfgGetNetBtn;
static HWND g_hCfgMacText, g_hCfgDevText, g_hCfgSwText;
static HWND g_hCfgIpText, g_hCfgSmText, g_hCfgGwText;
static HWND g_hCfgGwidText, g_hCfgCsqText;

/* 发现设备信息 */
static char g_dev_mac[32] = "";
static char g_dev_addr[INET_ADDRSTRLEN] = "";  /* 设备 UDP 源地址，用于单播 */
static char g_dev_ip[64] = "";
static char g_dev_sm[64] = "";
static char g_dev_gw[64] = "";
static char g_dev_gwid[32] = "";  /* GWID */
static char g_dev_name[64] = "";
static char g_dev_sw[32] = "";

/* 页面控件数组 (用于显示/隐藏) */
static HWND g_dataPage[64];
static int g_nDataPage = 0;
static HWND g_cfgPage[64];
static int g_nCfgPage = 0;

static char g_log_buf[LOG_BUF_MAX];
static int g_log_len = 0;

static char g_cfg_log_buf[LOG_BUF_MAX];
static int g_cfg_log_len = 0;

/* 字体 */
static HFONT g_hFont;
static HFONT g_hFontBold;
static HFONT g_hFontTitle;

/* ================================================================
 * 前向声明
 * ================================================================ */
static void update_stats(void);
static void update_telemetry(void);
static void update_connection_status(void);
static void add_history_entry(uint32_t nid, const char *type,
                              const uint8_t *data, uint16_t data_len);
static void send_ack(uint32_t nid);
static void send_data_frame(uint32_t nid, const uint8_t *data, uint16_t data_len);
static void log_append(const char *text);
static void log_hex(const char *prefix, const uint8_t *data, int len);

/* ================================================================
 * 工具函数
 * ================================================================ */

static void put_be32(uint8_t *buf, uint32_t val)
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

static void put_be16(uint8_t *buf, uint16_t val)
{
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

static uint32_t get_be32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | buf[3];
}

static uint16_t get_be16(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static void get_time_str(char *buf, size_t size)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, size, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
}

static void get_timestamp_str(char *buf, size_t size)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, size, "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/* 页面控件注册 */
static void reg_data(HWND h)
{
    if (h && g_nDataPage < 64) g_dataPage[g_nDataPage++] = h;
}

static void reg_cfg(HWND h)
{
    if (h && g_nCfgPage < 64) g_cfgPage[g_nCfgPage++] = h;
}

static void switch_tab(int idx)
{
    int i;
    for (i = 0; i < g_nDataPage; i++)
        ShowWindow(g_dataPage[i], idx == 0 ? SW_SHOW : SW_HIDE);
    for (i = 0; i < g_nCfgPage; i++)
        ShowWindow(g_cfgPage[i], idx == 1 ? SW_SHOW : SW_HIDE);
}

/* ================================================================
 * 日志系统
 * ================================================================ */
static void log_append(const char *text)
{
    char timebuf[16];
    get_time_str(timebuf, sizeof(timebuf));

    char line[1024];
    int len = snprintf(line, sizeof(line), "[%s] %s\r\n", timebuf, text);

    if (g_log_len + len >= LOG_BUF_MAX) {
        int half = g_log_len / 2;
        memmove(g_log_buf, g_log_buf + half, g_log_len - half);
        g_log_len -= half;
    }
    memcpy(g_log_buf + g_log_len, line, len);
    g_log_len += len;
    g_log_buf[g_log_len] = '\0';

    SetWindowTextA(g_hLogEdit, g_log_buf);
    int line_count = (int)SendMessageA(g_hLogEdit, EM_GETLINECOUNT, 0, 0);
    SendMessageA(g_hLogEdit, EM_LINESCROLL, 0, line_count);
}

static void log_hex(const char *prefix, const uint8_t *data, int len)
{
    char hex[1024];
    int offset = snprintf(hex, sizeof(hex), "%s (%d bytes): ", prefix, len);
    for (int i = 0; i < len && offset < (int)sizeof(hex) - 4; i++) {
        offset += snprintf(hex + offset, sizeof(hex) - offset, "%02X ", data[i]);
    }
    log_append(hex);
}

/* 配置页日志 */
static void cfg_log_append(const char *text)
{
    char timebuf[16];
    get_time_str(timebuf, sizeof(timebuf));

    char line[1024];
    int len = snprintf(line, sizeof(line), "[%s] %s\r\n", timebuf, text);

    if (g_cfg_log_len + len >= LOG_BUF_MAX) {
        int half = g_cfg_log_len / 2;
        memmove(g_cfg_log_buf, g_cfg_log_buf + half, g_cfg_log_len - half);
        g_cfg_log_len -= half;
    }
    memcpy(g_cfg_log_buf + g_cfg_log_len, line, len);
    g_cfg_log_len += len;
    g_cfg_log_buf[g_cfg_log_len] = '\0';

    SetWindowTextA(g_hCfgLogEdit, g_cfg_log_buf);
    int lc = (int)SendMessageA(g_hCfgLogEdit, EM_GETLINECOUNT, 0, 0);
    SendMessageA(g_hCfgLogEdit, EM_LINESCROLL, 0, lc);
}

/* ================================================================
 * 帧协议 — 组帧/解析
 * ================================================================ */

static int build_frame(uint8_t *out, size_t out_size, uint32_t nid,
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

static void send_ack(uint32_t nid)
{
    if (g_sock == INVALID_SOCKET) return;

    uint8_t buf[4 + 8];
    put_be32(buf, nid);
    int len = build_frame(buf + LORA_GATEWAY_PREFIX, sizeof(buf) - LORA_GATEWAY_PREFIX, nid, NULL, 0);
    if (len <= 0) return;

    send(g_sock, (const char *)buf, LORA_GATEWAY_PREFIX + len, 0);
    g_tx_count++;
    log_hex("TX ACK", buf, LORA_GATEWAY_PREFIX + len);
    update_stats();
}

static void send_data_frame(uint32_t nid, const uint8_t *data, uint16_t data_len)
{
    if (g_sock == INVALID_SOCKET) return;

    uint8_t buf[4 + 256];
    put_be32(buf, nid);
    int len = build_frame(buf + LORA_GATEWAY_PREFIX, sizeof(buf) - LORA_GATEWAY_PREFIX, nid, data, data_len);
    if (len <= 0) return;

    send(g_sock, (const char *)buf, LORA_GATEWAY_PREFIX + len, 0);
    g_tx_count++;
    log_hex("TX", buf, LORA_GATEWAY_PREFIX + len);
    add_history_entry(nid, "TX", data, data_len);
    update_stats();
}

static int parse_frame(const uint8_t *data, int len)
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
        log_append(dbg);
        log_hex("RX (bad CRC)", data, total);
        update_stats();
        return total;
    }

    if (g_nid != 0 && nid != g_nid) {
        log_hex("RX (NID mismatch)", data, total);
        return total;
    }

    /* 更新 NID 显示 */
    g_nid = nid;
    {
        char nid_buf[16];
        snprintf(nid_buf, sizeof(nid_buf), "%08X", nid);
        SetWindowTextA(g_hNidEdit, nid_buf);
    }

    g_rx_count++;
    const uint8_t *payload = data + LORA_FRAME_HEADER_SIZE;

    if (data_len == 0) {
        log_append("RX ACK (empty payload)");
        add_history_entry(nid, "ACK", NULL, 0);
    } else if (data_len == 8 &&
               payload[5] == 0xFF && payload[6] == 0xFF && payload[7] == 0xFF) {
        g_last_x = (int16_t)get_be16(payload);
        g_last_y = (int16_t)get_be16(payload + 2);
        g_last_btn = payload[4] & 0x01;

        char desc[128];
        snprintf(desc, sizeof(desc), "Telemetry: X=%.1f Y=%.1f Btn=%s",
                 g_last_x / 10.0, g_last_y / 10.0,
                 g_last_btn ? "Released" : "Pressed");
        log_append(desc);

        char detail[64];
        snprintf(detail, sizeof(detail), "X=%.1f Y=%.1f Btn=%d",
                 g_last_x / 10.0, g_last_y / 10.0, g_last_btn);
        add_history_entry(nid, "Telemetry",
                          (const uint8_t *)detail, (uint16_t)strlen(detail));

        update_telemetry();

        if (g_auto_ack) {
            send_ack(nid);
        }
    } else if (data_len >= 2) {
        uint16_t can_id = get_be16(payload);
        char desc[256];
        int off = snprintf(desc, sizeof(desc), "Scanner CAN=0x%03X Data:", can_id);
        for (int i = 2; i < data_len && off < (int)sizeof(desc) - 4; i++) {
            off += snprintf(desc + off, sizeof(desc) - off, " %02X", payload[i]);
        }
        log_append(desc);
        add_history_entry(nid, "Scanner", payload, data_len);
    } else {
        log_hex("RX Data", data, total);
        add_history_entry(nid, "Data", payload, data_len);
    }

    log_hex("RX", data, total);
    update_stats();
    return total;
}

/* ================================================================
 * UI 更新函数
 * ================================================================ */

static void update_connection_status(void)
{
    if (g_connected) {
        SetWindowTextA(g_hStatusText, "  Connected");
        EnableWindow(g_hConnectBtn, FALSE);
        EnableWindow(g_hDisconnectBtn, TRUE);
    } else {
        SetWindowTextA(g_hStatusText, "  Disconnected");
        EnableWindow(g_hConnectBtn, TRUE);
        EnableWindow(g_hDisconnectBtn, FALSE);
    }
}

static void update_stats(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "RX: %d", g_rx_count);
    SetWindowTextA(g_hRxCount, buf);
    snprintf(buf, sizeof(buf), "TX: %d", g_tx_count);
    SetWindowTextA(g_hTxCount, buf);
    snprintf(buf, sizeof(buf), "ERR: %d", g_err_count);
    SetWindowTextA(g_hErrCount, buf);
}

static void update_telemetry(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "X Angle: %.1f deg", g_last_x / 10.0);
    SetWindowTextA(g_hXText, buf);
    snprintf(buf, sizeof(buf), "Y Angle: %.1f deg", g_last_y / 10.0);
    SetWindowTextA(g_hYText, buf);
    snprintf(buf, sizeof(buf), "Button: %s",
             g_last_btn ? "Released" : "Pressed");
    SetWindowTextA(g_hBtnText, buf);
}

static void add_history_entry(uint32_t nid, const char *type,
                              const uint8_t *data, uint16_t data_len)
{
    char timebuf[16];
    get_timestamp_str(timebuf, sizeof(timebuf));

    char nid_str[16];
    snprintf(nid_str, sizeof(nid_str), "%08X", nid);

    char data_str[256] = "";
    if (data_len > 0 && data) {
        BOOL printable = TRUE;
        for (int i = 0; i < data_len; i++) {
            if (data[i] != '\0' && (data[i] < 0x20 || data[i] > 0x7E)) {
                printable = FALSE;
                break;
            }
        }
        if (printable) {
            memcpy(data_str, data, data_len);
            data_str[data_len] = '\0';
        } else {
            int off = 0;
            for (int i = 0; i < data_len && off < (int)sizeof(data_str) - 4; i++) {
                off += snprintf(data_str + off, sizeof(data_str) - off,
                                "%02X ", data[i]);
            }
        }
    }

    LVITEMA item = {0};
    item.mask = LVIF_TEXT;
    item.iItem = 0;
    item.iSubItem = 0;
    item.pszText = timebuf;
    int idx = ListView_InsertItem(g_hHistoryList, &item);

    ListView_SetItemText(g_hHistoryList, idx, 1, nid_str);
    ListView_SetItemText(g_hHistoryList, idx, 2, (LPSTR)type);
    ListView_SetItemText(g_hHistoryList, idx, 3, data_str);

    int count = ListView_GetItemCount(g_hHistoryList);
    if (count > HISTORY_MAX) {
        ListView_DeleteItem(g_hHistoryList, count - 1);
    }
}

/* ================================================================
 * TCP 连接管理
 * ================================================================ */

static void do_connect(void)
{
    char ip[64];
    char port_str[16];

    GetWindowTextA(g_hIpEdit, ip, sizeof(ip));
    GetWindowTextA(g_hPortEdit, port_str, sizeof(port_str));

    if (strlen(ip) == 0 || strlen(port_str) == 0) {
        MessageBoxA(g_hwndMain, "Please enter IP and Port", "Error", MB_OK);
        return;
    }

    int port = atoi(port_str);
    if (port <= 0 || port > 65535) {
        MessageBoxA(g_hwndMain, "Invalid port number", "Error", MB_OK);
        return;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == INVALID_SOCKET) {
        MessageBoxA(g_hwndMain, "Failed to create socket", "Error", MB_OK);
        return;
    }

    u_long mode = 1;
    ioctlsocket(g_sock, FIONBIO, &mode);

    WSAAsyncSelect(g_sock, g_hwndMain, WM_SOCKET,
                   FD_READ | FD_CLOSE | FD_CONNECT);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    SetWindowTextA(g_hStatusText, "  Connecting...");
    EnableWindow(g_hConnectBtn, FALSE);

    int ret = connect(g_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        SetWindowTextA(g_hStatusText, "  Connect failed");
        EnableWindow(g_hConnectBtn, TRUE);
        MessageBoxA(g_hwndMain, "Connect failed", "Error", MB_OK);
    }
}

static void do_disconnect(void)
{
    if (g_sock != INVALID_SOCKET) {
        WSAAsyncSelect(g_sock, g_hwndMain, 0, 0);
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    g_connected = FALSE;
    g_rx_len = 0;
    update_connection_status();
    log_append("Disconnected");
}

static void process_rx_data(void)
{
    char buf[2048];
    int bytes = recv(g_sock, buf, sizeof(buf), 0);
    if (bytes <= 0) return;

    if (g_rx_len + bytes > RX_BUF_MAX) {
        g_rx_len = 0;
    }
    memcpy(g_rx_buf + g_rx_len, buf, bytes);
    g_rx_len += bytes;

    while (g_rx_len >= LORA_FRAME_OVERHEAD) {
        int consumed = parse_frame(g_rx_buf, g_rx_len);
        if (consumed == 0) break;
        if (consumed > g_rx_len) consumed = g_rx_len;
        g_rx_len -= consumed;
        if (g_rx_len > 0) {
            memmove(g_rx_buf, g_rx_buf + consumed, g_rx_len);
        }
    }
}

/* ================================================================
 * UDP 配置 — USR-LG210-L 网关发现与配置
 *
 * 流程:
 *   1. SEARCH 广播发现设备 → 获取 MAC/型号/版本
 *   2. GETPARA(NETDEV) 获取网络参数 → IP/子网掩码/网关
 *   3. GETPARA(AT) 发送 AT 指令 → 配置 LoRa 参数
 *
 * 使用独立线程 + 阻塞 socket + 5s 超时，与 Python 脚本一致。
 * ================================================================ */

#define UDP_BROADCAST_PORT 1566
#define MAX_UDP_IFACES     16

/* 构建 USR1566{json}USR1566 格式 payload */
static int udp_wrap_json(cJSON *root, uint8_t *out, int out_size)
{
    char *json = cJSON_PrintUnformatted(root);
    if (!json) return -1;

    int jlen = (int)strlen(json);
    int total = 7 + jlen + 7;
    if (total >= out_size) { cJSON_free(json); return -1; }

    memcpy(out, "USR1566", 7);
    memcpy(out + 7, json, jlen);
    memcpy(out + 7 + jlen, "USR1566", 7);
    cJSON_free(json);
    return total;
}

/* 线程参数：携带要发送的 payload + 可选目标 IP */
typedef struct {
    int plen;
    uint8_t payload[2048];
    char target_ip[INET_ADDRSTRLEN]; /* 非空则单播到此 IP */
} udp_work_t;

/* WM_UDP_RX 消息携带的数据块 */
typedef struct {
    char from_ip[INET_ADDRSTRLEN];
    char data[1]; /* 变长 */
} udp_rx_msg_t;

/* UDP 工作线程 — 有 target_ip 时单播，否则枚举所有活跃网卡广播 */
static DWORD WINAPI udp_worker(LPVOID param)
{
    udp_work_t *work = (udp_work_t *)param;
    int got_response = 0;

    SOCKET socks[MAX_UDP_IFACES];
    int n_socks = 0;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(UDP_BROADCAST_PORT);

    if (work->target_ip[0]) {
        /* ---- 单播模式：向已发现的设备 IP 发送 ---- */
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            PostMessage(g_hwndMain, WM_UDP_LOG, 0,
                        (LPARAM)_strdup("Failed to create UDP socket"));
            free(work);
            return 0;
        }

        inet_pton(AF_INET, work->target_ip, &dest.sin_addr);

        int ret = sendto(sock, (const char *)work->payload, work->plen, 0,
                         (struct sockaddr *)&dest, sizeof(dest));
        if (ret == SOCKET_ERROR) {
            char err[128];
            snprintf(err, sizeof(err), "sendto %s failed: %d",
                     work->target_ip, WSAGetLastError());
            PostMessage(g_hwndMain, WM_UDP_LOG, 0, (LPARAM)_strdup(err));
            closesocket(sock);
            free(work);
            return 0;
        }

        char log[1100];
        snprintf(log, sizeof(log), "TX -> %s | %.*s",
                 work->target_ip, work->plen, (const char *)work->payload);
        PostMessage(g_hwndMain, WM_UDP_LOG, 0, (LPARAM)_strdup(log));

        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
        socks[n_socks++] = sock;
    } else {
        /* ---- 广播模式：枚举所有活跃 IPv4 网卡分别发送 ---- */
        ULONG bufLen = 0;
        GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_FRIENDLY_NAME,
            NULL, NULL, &bufLen);

        PIP_ADAPTER_ADDRESSES adapters = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
        if (!adapters) {
            PostMessage(g_hwndMain, WM_UDP_LOG, 0,
                        (LPARAM)_strdup("Failed to enumerate adapters"));
            free(work);
            return 0;
        }

        if (GetAdaptersAddresses(AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_FRIENDLY_NAME,
                NULL, adapters, &bufLen) != ERROR_SUCCESS) {
            PostMessage(g_hwndMain, WM_UDP_LOG, 0,
                        (LPARAM)_strdup("GetAdaptersAddresses failed"));
            free(adapters);
            free(work);
            return 0;
        }

        struct in_addr if_ips[MAX_UDP_IFACES];
        int n_if = 0;

        for (PIP_ADAPTER_ADDRESSES aa = adapters; aa && n_if < MAX_UDP_IFACES; aa = aa->Next) {
            if (aa->OperStatus != IfOperStatusUp) continue;
            if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS ua = aa->FirstUnicastAddress;
                 ua && n_if < MAX_UDP_IFACES; ua = ua->Next) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ua->Address.lpSockaddr;
                if (sa->sin_family != AF_INET) continue;
                if_ips[n_if++] = sa->sin_addr;
            }
        }
        free(adapters);

        if (n_if == 0) {
            PostMessage(g_hwndMain, WM_UDP_LOG, 0,
                        (LPARAM)_strdup("No active IPv4 interfaces found"));
            free(work);
            return 0;
        }

        dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        for (int i = 0; i < n_if; i++) {
            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) continue;

            BOOL bcast = TRUE;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                       (const char *)&bcast, sizeof(bcast));

            struct sockaddr_in local;
            memset(&local, 0, sizeof(local));
            local.sin_family = AF_INET;
            local.sin_addr = if_ips[i];
            if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
                closesocket(sock);
                continue;
            }

            int ret = sendto(sock, (const char *)work->payload, work->plen, 0,
                             (struct sockaddr *)&dest, sizeof(dest));
            if (ret == SOCKET_ERROR) {
                char ip_buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &if_ips[i], ip_buf, sizeof(ip_buf));
                char err_buf[128];
                snprintf(err_buf, sizeof(err_buf), "sendto failed on %s: %d",
                         ip_buf, WSAGetLastError());
                PostMessage(g_hwndMain, WM_UDP_LOG, 0, (LPARAM)_strdup(err_buf));
                closesocket(sock);
                continue;
            }

            char ip_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &if_ips[i], ip_buf, sizeof(ip_buf));
            char log[1100];
            snprintf(log, sizeof(log), "TX (%s) -> %.*s",
                     ip_buf, work->plen, (const char *)work->payload);
            PostMessage(g_hwndMain, WM_UDP_LOG, 0, (LPARAM)_strdup(log));

            u_long mode = 1;
            ioctlsocket(sock, FIONBIO, &mode);

            socks[n_socks++] = sock;
        }
    }

    if (n_socks == 0) {
        PostMessage(g_hwndMain, WM_UDP_LOG, 0,
                    (LPARAM)_strdup("Failed to send on any interface"));
        free(work);
        return 0;
    }

    /* --- select 多路接收，总超时 5 秒 --- */
    DWORD start = GetTickCount();
    char buf[2048];

    while (1) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= 5000) break;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        for (int i = 0; i < n_socks; i++)
            FD_SET(socks[i], &read_fds);

        int sel = select(0, &read_fds, NULL, NULL, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel > 0) {
            for (int i = 0; i < n_socks; i++) {
                if (FD_ISSET(socks[i], &read_fds)) {
                    struct sockaddr_in from;
                    int fromlen = sizeof(from);
                    int bytes = recvfrom(socks[i], buf, sizeof(buf) - 1, 0,
                                         (struct sockaddr *)&from, &fromlen);
                    if (bytes > 0) {
                        buf[bytes] = '\0';
                        /* 构造携带源 IP 的消息 */
                        int msg_size = (int)offsetof(udp_rx_msg_t, data) + bytes + 1;
                        udp_rx_msg_t *msg = (udp_rx_msg_t *)malloc(msg_size);
                        if (msg) {
                            inet_ntop(AF_INET, &from.sin_addr, msg->from_ip, sizeof(msg->from_ip));
                            memcpy(msg->data, buf, bytes + 1);
                            PostMessage(g_hwndMain, WM_UDP_RX, 0, (LPARAM)msg);
                        }
                        got_response = 1;
                    }
                }
            }
        }
    }

    if (!got_response) {
        PostMessage(g_hwndMain, WM_UDP_LOG, 0,
                    (LPARAM)_strdup("No response (timeout 5s)"));
    }

    for (int i = 0; i < n_socks; i++)
        closesocket(socks[i]);
    free(work);
    return 0;
}

/* 广播发送 */
static void udp_send_raw(const uint8_t *payload, int plen)
{
    udp_work_t *work = (udp_work_t *)calloc(1, sizeof(udp_work_t));
    if (!work) return;
    work->plen = plen;
    memcpy(work->payload, payload, plen);

    CloseHandle(CreateThread(NULL, 0, udp_worker, work, 0, NULL));
}

/* 单播发送到目标 IP */
static void udp_send_unicast(const char *ip, const uint8_t *payload, int plen)
{
    udp_work_t *work = (udp_work_t *)calloc(1, sizeof(udp_work_t));
    if (!work) return;
    work->plen = plen;
    memcpy(work->payload, payload, plen);
    snprintf(work->target_ip, sizeof(work->target_ip), "%s", ip);

    CloseHandle(CreateThread(NULL, 0, udp_worker, work, 0, NULL));
}

/* Step 1: 搜索设备 */
static void do_cfg_search(void)
{

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VER", "1.0");
    cJSON_AddStringToObject(root, "MSG", "SEARCH");
    cJSON_AddStringToObject(root, "TYPE", "LORA");

    uint8_t payload[1024];
    int plen = udp_wrap_json(root, payload, sizeof(payload));
    cJSON_Delete(root);

    if (plen <= 0) { cfg_log_append("Failed to build payload"); return; }

    udp_send_raw(payload, plen);
}

/* Step 2: 获取网络参数 */
static void do_cfg_get_net(void)
{
    if (strlen(g_dev_mac) == 0) {
        MessageBoxA(g_hwndMain, "Please search devices first", "Error", MB_OK);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VER", "1.0");
    cJSON_AddStringToObject(root, "MSG", "GETPARA");
    cJSON_AddStringToObject(root, "TYPE", "JSON");
    cJSON_AddStringToObject(root, "CMD", "NETDEV");
    cJSON_AddStringToObject(root, "USER", "admin");
    cJSON_AddStringToObject(root, "PSW", "admin");
    cJSON_AddStringToObject(root, "MAC", g_dev_mac);

    uint8_t payload[1024];
    int plen = udp_wrap_json(root, payload, sizeof(payload));
    cJSON_Delete(root);

    if (plen <= 0) { cfg_log_append("Failed to build payload"); return; }

    /* 有设备地址时单播，否则广播 */
    if (g_dev_addr[0])
        udp_send_unicast(g_dev_addr, payload, plen);
    else
        udp_send_raw(payload, plen);
}

/* 发送 AT 指令 (GETPARA, TYPE=AT) */
static void udp_send_at_cmd(const char *cmd)
{
    /* 确保 \r\n 结尾 */
    size_t clen = strlen(cmd);
    char full_cmd[256];
    if (clen >= 2 && cmd[clen - 2] == '\r' && cmd[clen - 1] == '\n') {
        snprintf(full_cmd, sizeof(full_cmd), "%s", cmd);
    } else {
        snprintf(full_cmd, sizeof(full_cmd), "%s\r\n", cmd);
    }

    const char *mac = strlen(g_dev_mac) > 0 ? g_dev_mac : "D4AD20ED63C4";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VER", "1.0");
    cJSON_AddStringToObject(root, "MSG", "GETPARA");
    cJSON_AddStringToObject(root, "TYPE", "AT");
    cJSON_AddStringToObject(root, "CMD", full_cmd);
    cJSON_AddStringToObject(root, "USER", "admin");
    cJSON_AddStringToObject(root, "PSW", "admin");
    cJSON_AddStringToObject(root, "MAC", mac);

    uint8_t payload[1024];
    int plen = udp_wrap_json(root, payload, sizeof(payload));
    cJSON_Delete(root);

    if (plen <= 0) { cfg_log_append("Failed to build payload"); return; }

    /* 有设备地址时单播，否则广播 */
    if (g_dev_addr[0])
        udp_send_unicast(g_dev_addr, payload, plen);
    else
        udp_send_raw(payload, plen);
}

/* 处理 UDP 接收响应 — 在 UI 线程中被 WM_UDP_RX 调用 */
static void udp_process_response(const char *from_ip, const char *raw)
{
    /* 提取 JSON */
    const char *start = strchr(raw, '{');
    const char *end = strrchr(raw, '}');
    if (!start || !end || end <= start) { cfg_log_append(raw); return; }

    /* 复制 JSON 子串用于解析 */
    int jlen = (int)(end - start + 1);
    char json_buf[2048];
    if (jlen >= (int)sizeof(json_buf)) return;
    memcpy(json_buf, start, jlen);
    json_buf[jlen] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        cfg_log_append("RX <- (parse error)");
        return;
    }

    const char *msg = "";
    cJSON *msg_item = cJSON_GetObjectItemCaseSensitive(root, "MSG");
    if (msg_item && cJSON_IsString(msg_item)) msg = msg_item->valuestring;

    /* ACK-SEARCH: 设备发现响应 */
    if (strcmp(msg, "ACK-SEARCH") == 0) {
        /* 保存设备 UDP 源地址，后续通信用单播 */
        snprintf(g_dev_addr, sizeof(g_dev_addr), "%s", from_ip);

        cJSON *mac = cJSON_GetObjectItemCaseSensitive(root, "MAC");
        cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "DEV");
        cJSON *sver = cJSON_GetObjectItemCaseSensitive(root, "SVER");

        if (mac && cJSON_IsString(mac)) {
            snprintf(g_dev_mac, sizeof(g_dev_mac), "%s", mac->valuestring);
            char txt[128]; snprintf(txt, sizeof(txt), "MAC: %s", g_dev_mac);
            SetWindowTextA(g_hCfgMacText, txt);
        }
        if (dev && cJSON_IsString(dev)) {
            snprintf(g_dev_name, sizeof(g_dev_name), "%s", dev->valuestring);
            char txt[128]; snprintf(txt, sizeof(txt), "Device: %s", g_dev_name);
            SetWindowTextA(g_hCfgDevText, txt);
        }
        if (sver && cJSON_IsString(sver)) {
            snprintf(g_dev_sw, sizeof(g_dev_sw), "%s", sver->valuestring);
            char txt[128]; snprintf(txt, sizeof(txt), "SW: %s", g_dev_sw);
            SetWindowTextA(g_hCfgSwText, txt);
        }

        cfg_log_append("Device found!");
    }

    /* ACK-GETPARA + CMD=NETDEV: 网络参数响应 */
    if (strcmp(msg, "ACK-GETPARA") == 0) {
        cJSON *cmd_obj = cJSON_GetObjectItemCaseSensitive(root, "CMD");
        if (cmd_obj && cJSON_IsObject(cmd_obj)) {
            cJSON *ip = cJSON_GetObjectItemCaseSensitive(cmd_obj, "IP");
            cJSON *sm = cJSON_GetObjectItemCaseSensitive(cmd_obj, "SM");
            cJSON *gw = cJSON_GetObjectItemCaseSensitive(cmd_obj, "GW");

            if (ip && cJSON_IsString(ip)) {
                snprintf(g_dev_ip, sizeof(g_dev_ip), "%s", ip->valuestring);
                char txt[128]; snprintf(txt, sizeof(txt), "IP: %s", g_dev_ip);
                SetWindowTextA(g_hCfgIpText, txt);
            }
            if (sm && cJSON_IsString(sm)) {
                snprintf(g_dev_sm, sizeof(g_dev_sm), "%s", sm->valuestring);
                char txt[128]; snprintf(txt, sizeof(txt), "Mask: %s", g_dev_sm);
                SetWindowTextA(g_hCfgSmText, txt);
            }
            if (gw && cJSON_IsString(gw)) {
                snprintf(g_dev_gw, sizeof(g_dev_gw), "%s", gw->valuestring);
                char txt[128]; snprintf(txt, sizeof(txt), "GW: %s", g_dev_gw);
                SetWindowTextA(g_hCfgGwText, txt);
            }

            cfg_log_append("Network parameters received");
        } else {
            /* AT 指令响应 — CMD 是字符串 */
            cJSON *cmd_str = cJSON_GetObjectItemCaseSensitive(root, "CMD");
            if (cmd_str && cJSON_IsString(cmd_str)) {
                const char *val = cmd_str->valuestring;
                char log[600];
                snprintf(log, sizeof(log), "RX <- CMD: %s", val);
                cfg_log_append(log);

                /* 解析 GWID 响应: 匹配 "GWID:" 或 "+GWID:" */
                const char *p = strstr(val, "GWID:");
                if (p) {
                    p += 5; /* skip "GWID:" */
                    int i = 0;
                    while (p[i] && p[i] != '\r' && p[i] != '\n' && p[i] != ' ' && i < 15) i++;
                    snprintf(g_dev_gwid, sizeof(g_dev_gwid), "%.*s", i, p);
                    char txt[128];
                    snprintf(txt, sizeof(txt), "GWID: %s", g_dev_gwid);
                    SetWindowTextA(g_hCfgGwidText, txt);
                }

                /* 解析 CSQ 响应: "+CSQ:<net>,<signal>" */
                const char *csq = strstr(val, "+CSQ:");
                if (csq) {
                    csq += 5; /* skip "+CSQ:" */
                    int net_val = 0, sig_val = 0;
                    if (sscanf(csq, "%d,%d", &net_val, &sig_val) == 2) {
                        const char *net_str = (net_val == 2) ? "2G" :
                                              (net_val == 3) ? "3G" :
                                              (net_val == 4) ? "4G" : "?";
                        char txt[128];
                        snprintf(txt, sizeof(txt), "Signal: %s (%d)", net_str, sig_val);
                        SetWindowTextA(g_hCfgCsqText, txt);
                    }
                }
            }
        }
    }

    /* 格式化打印完整 JSON */
    char *fmt = cJSON_Print(root);
    if (fmt) {
        cfg_log_append(fmt);
        cJSON_free(fmt);
    }

    cJSON_Delete(root);
}

static void do_cfg_send(void)
{
    char cmd[256];
    GetWindowTextA(g_hCfgCmdEdit, cmd, sizeof(cmd));
    if (strlen(cmd) == 0) {
        MessageBoxA(g_hwndMain, "Please enter AT command", "Error", MB_OK);
        return;
    }
    udp_send_at_cmd(cmd);
}

static void do_cfg_quick(const char *cmd)
{
    SetWindowTextA(g_hCfgCmdEdit, cmd);
    udp_send_at_cmd(cmd);
}

/* ================================================================
 * 手动发送
 * ================================================================ */

static int parse_hex_input(const char *input, uint8_t *out, int max_len)
{
    int count = 0;
    const char *p = input;

    while (*p && count < max_len) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char *end;
        long val = strtol(p, &end, 16);
        if (end == p) break;
        if (val < 0 || val > 255) break;

        out[count++] = (uint8_t)val;
        p = end;
    }
    return count;
}

static void do_manual_send(void)
{
    char input[512];
    GetWindowTextA(g_hSendEdit, input, sizeof(input));

    uint8_t data[256];
    int len = parse_hex_input(input, data, sizeof(data));
    if (len == 0) {
        MessageBoxA(g_hwndMain,
                    "Please enter hex data (e.g. 01 02 FF)",
                    "Error", MB_OK);
        return;
    }

    uint32_t nid = g_nid ? g_nid : 0x00000001;
    send_data_frame(nid, data, (uint16_t)len);
}

static void do_manual_ack(void)
{
    uint32_t nid = g_nid ? g_nid : 0x00000001;
    send_ack(nid);
}

/* ================================================================
 * 控件创建辅助
 * ================================================================ */

static HWND make_static(HWND parent, const char *text,
                        int x, int y, int w, int h, HFONT font)
{
    HWND hw = CreateWindowA("STATIC", text,
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            x, y, w, h, parent, NULL, g_hInst, NULL);
    SendMessage(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static HWND make_edit(HWND parent, const char *text, int id,
                      int x, int y, int w, int h)
{
    HWND hw = CreateWindowA("EDIT", text,
                            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                            x, y, w, h, parent, (HMENU)(LONG_PTR)id,
                            g_hInst, NULL);
    SendMessage(hw, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return hw;
}

static HWND make_button(HWND parent, const char *text, int id,
                        int x, int y, int w, int h)
{
    HWND hw = CreateWindowA("BUTTON", text,
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            x, y, w, h, parent, (HMENU)(LONG_PTR)id,
                            g_hInst, NULL);
    SendMessage(hw, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return hw;
}

static HWND make_groupbox(HWND parent, const char *text,
                          int x, int y, int w, int h)
{
    HWND hw = CreateWindowA("BUTTON", text,
                            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                            x, y, w, h, parent, NULL, g_hInst, NULL);
    SendMessage(hw, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return hw;
}

/* ================================================================
 * Tab 页面创建
 * ================================================================ */

static void create_data_page(HWND hwnd, RECT *pageRc)
{
    const int PX = pageRc->left + 6;
    const int PW = (pageRc->right - pageRc->left) - 12;
    const int RH = 28;
    const int GAP = 6;
    int y = pageRc->top + 4;

    /* ── 1. Connection ── */
    int conn_h = RH * 2 + 30;
    reg_data(make_groupbox(hwnd, " Connection ", PX, y, PW, conn_h));

    int cx = PX + 12;
    int cy = y + 20;

    reg_data(make_static(hwnd, "IP:", cx, cy + 3, 22, RH, g_hFont));
    reg_data(g_hIpEdit = make_edit(hwnd, "192.168.2.100", IDC_IP_EDIT,
                                   cx + 26, cy, 150, RH));
    reg_data(make_static(hwnd, "Port:", cx + 186, cy + 3, 32, RH, g_hFont));
    reg_data(g_hPortEdit = make_edit(hwnd, "1234", IDC_PORT_EDIT,
                                     cx + 222, cy, 60, RH));
    reg_data(g_hConnectBtn = make_button(hwnd, "Connect", IDC_CONNECT_BTN,
                                         cx + 296, cy, 80, RH));
    reg_data(g_hDisconnectBtn = make_button(hwnd, "Disconnect", IDC_DISCONNECT_BTN,
                                            cx + 382, cy, 90, RH));
    EnableWindow(g_hDisconnectBtn, FALSE);
    reg_data(g_hStatusText = make_static(hwnd, "  Disconnected",
                                         cx + 486, cy + 3, 130, RH, g_hFontBold));

    /* 第二行: NID + Auto ACK */
    cy += RH + 4;
    reg_data(make_static(hwnd, "NID:", cx, cy + 3, 28, RH, g_hFont));
    reg_data(g_hNidEdit = make_edit(hwnd, "--------", IDC_NID_EDIT,
                                    cx + 32, cy, 90, RH));
    SendMessage(g_hNidEdit, EM_SETREADONLY, TRUE, 0);

    reg_data(g_hAutoAckCheck = CreateWindowA("BUTTON", "Auto ACK",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            cx + 230, cy + 2, 100, RH,
            hwnd, (HMENU)(LONG_PTR)IDC_AUTO_ACK_CHECK, g_hInst, NULL));
    SendMessage(g_hAutoAckCheck, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessage(g_hAutoAckCheck, BM_SETCHECK, BST_CHECKED, 0);

    y += conn_h + GAP;

    /* ── 2. Telemetry + Raw Log ── */
    int mid_h = RH * 4 + 30;
    int telem_w = 280;

    reg_data(make_groupbox(hwnd, " Telemetry ", PX, y, telem_w, mid_h));

    int tx = PX + 14;
    int ty = y + 20;
    reg_data(g_hXText = make_static(hwnd, "X Angle: -- deg", tx, ty, telem_w - 20, RH, g_hFont));
    ty += RH;
    reg_data(g_hYText = make_static(hwnd, "Y Angle: -- deg", tx, ty, telem_w - 20, RH, g_hFont));
    ty += RH;
    reg_data(g_hBtnText = make_static(hwnd, "Button: --", tx, ty, telem_w - 20, RH, g_hFont));
    ty += RH;
    reg_data(g_hRxCount = make_static(hwnd, "RX: 0", tx, ty, 80, RH, g_hFont));
    reg_data(g_hTxCount = make_static(hwnd, "TX: 0", tx + 85, ty, 80, RH, g_hFont));
    reg_data(g_hErrCount = make_static(hwnd, "ERR: 0", tx + 170, ty, 80, RH, g_hFont));

    int log_x = PX + telem_w + GAP;
    int log_w = PW - telem_w - GAP;
    reg_data(make_groupbox(hwnd, " Raw Log (Hex) ", log_x, y, log_w, mid_h));

    reg_data(g_hLogEdit = CreateWindowA("EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            log_x + 10, y + 20, log_w - 20, mid_h - 30,
            hwnd, (HMENU)(LONG_PTR)IDC_LOG_EDIT, g_hInst, NULL));
    SendMessage(g_hLogEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    y += mid_h + GAP;

    /* ── 3. Manual Send ── */
    int send_h = RH + 32;
    reg_data(make_groupbox(hwnd, " Manual Send ", PX, y, PW, send_h));

    int sx = PX + 12;
    int sy = y + 18;
    reg_data(make_static(hwnd, "Hex:", sx, sy + 3, 30, RH, g_hFont));
    reg_data(g_hSendEdit = make_edit(hwnd, "", IDC_SEND_EDIT, sx + 34, sy, 360, RH));
    reg_data(g_hSendBtn = make_button(hwnd, "Send", IDC_SEND_BTN, sx + 400, sy, 65, RH));
    reg_data(g_hSendAckBtn = make_button(hwnd, "Send ACK", IDC_SEND_ACK_BTN, sx + 471, sy, 85, RH));
    reg_data(g_hClearLogBtn = make_button(hwnd, "Clear Log", IDC_CLEAR_LOG_BTN, sx + 562, sy, 80, RH));

    y += send_h + GAP;

    /* ── 4. History ListView ── */
    int hist_h = pageRc->bottom - y - 4;
    if (hist_h < 60) hist_h = 60;

    reg_data(g_hHistoryList = CreateWindowA(WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
            PX, y, PW, hist_h,
            hwnd, (HMENU)(LONG_PTR)IDC_HISTORY_LIST, g_hInst, NULL));
    SendMessage(g_hHistoryList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    int lv_w = PW - 20;
    LVCOLUMNA col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;

    col.cx = 110;                        col.pszText = "Time";
    ListView_InsertColumn(g_hHistoryList, 0, &col);
    col.cx = 90;                         col.pszText = "NID";
    ListView_InsertColumn(g_hHistoryList, 1, &col);
    col.cx = 80;                         col.pszText = "Type";
    ListView_InsertColumn(g_hHistoryList, 2, &col);
    col.cx = lv_w - 110 - 90 - 80;      col.pszText = "Data";
    ListView_InsertColumn(g_hHistoryList, 3, &col);

    ListView_SetExtendedListViewStyle(g_hHistoryList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
}

static void create_config_page(HWND hwnd, RECT *pageRc)
{
    const int PX = pageRc->left + 6;
    const int PW = (pageRc->right - pageRc->left) - 12;
    const int RH = 28;
    const int GAP = 6;
    int y = pageRc->top + 4;
    int cx = PX + 12;

    /* ── 1. Device Discovery ── */
    int dev_h = RH * 2 + 34;
    reg_cfg(make_groupbox(hwnd, " Device Discovery ", PX, y, PW, dev_h));

    int cy = y + 18;
    reg_cfg(g_hCfgSearchBtn = make_button(hwnd, "Search Devices", IDC_CFG_SEARCH_BTN,
                                          cx, cy, 120, RH));
    reg_cfg(g_hCfgGetNetBtn = make_button(hwnd, "Get Network", IDC_CFG_GETNET_BTN,
                                          cx + 128, cy, 120, RH));
    reg_cfg(g_hCfgQueryGwid = make_button(hwnd, "Query GWID", IDC_CFG_QUERY_GWID,
                                           cx + 256, cy, 120, RH));
    reg_cfg(g_hCfgQueryCsq = make_button(hwnd, "Query Signal", IDC_CFG_QUERY_CSQ,
                                          cx + 384, cy, 120, RH));

    cy += RH + 4;
    reg_cfg(g_hCfgMacText = make_static(hwnd, "MAC: --", cx, cy, 220, RH, g_hFont));
    reg_cfg(g_hCfgDevText = make_static(hwnd, "Device: --", cx + 228, cy, 220, RH, g_hFont));
    reg_cfg(g_hCfgSwText = make_static(hwnd, "SW: --", cx + 456, cy, 200, RH, g_hFont));

    y += dev_h + GAP;

    /* ── 2. Network Info ── */
    int net_h = RH * 2 + 34;
    reg_cfg(make_groupbox(hwnd, " Network ", PX, y, PW, net_h));

    cy = y + 18;
    reg_cfg(g_hCfgIpText = make_static(hwnd, "IP: --", cx, cy, 240, RH, g_hFont));
    reg_cfg(g_hCfgSmText = make_static(hwnd, "Mask: --", cx + 248, cy, 220, RH, g_hFont));
    reg_cfg(g_hCfgGwText = make_static(hwnd, "GW: --", cx + 476, cy, 200, RH, g_hFont));

    cy += RH + 4;
    reg_cfg(g_hCfgGwidText = make_static(hwnd, "GWID: --", cx, cy, 240, RH, g_hFont));
    reg_cfg(g_hCfgCsqText = make_static(hwnd, "Signal: --", cx + 248, cy, 240, RH, g_hFont));

    y += net_h + GAP;

    /* ── 3. AT Command ── */
    int cmd_h = RH + 30;
    reg_cfg(make_groupbox(hwnd, " AT Command (UDP :1566) ", PX, y, PW, cmd_h));

    cy = y + 18;
    reg_cfg(make_static(hwnd, "CMD:", cx, cy + 3, 32, RH, g_hFont));
    reg_cfg(g_hCfgCmdEdit = make_edit(hwnd, "AT+VER?\r\n", IDC_CFG_CMD_EDIT,
                                      cx + 36, cy, PW - 120, RH));
    reg_cfg(g_hCfgSendBtn = make_button(hwnd, "Send", IDC_CFG_SEND_BTN,
                                        cx + PW - 76, cy, 65, RH));

    y += cmd_h + GAP;

    /* ── 4. Quick Commands ── */
    int qk_h = RH + 30;
    reg_cfg(make_groupbox(hwnd, " Quick Commands ", PX, y, PW, qk_h));

    cy = y + 18;
    int bw = 120;
    int bx = cx;

    reg_cfg(g_hCfgQueryVer = make_button(hwnd, "Query Version", IDC_CFG_QUERY_VER,
                                         bx, cy, bw, RH));

    y += qk_h + GAP;

    /* ── 5. Response Log ── */
    int log_h = pageRc->bottom - y - RH - GAP - 4;
    if (log_h < 60) log_h = 60;
    reg_cfg(make_groupbox(hwnd, " Response ", PX, y, PW, log_h + RH + 4));

    reg_cfg(g_hCfgLogEdit = CreateWindowA("EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            PX + 10, y + 20, PW - 20, log_h,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_LOG_EDIT, g_hInst, NULL));
    SendMessage(g_hCfgLogEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    cy = y + log_h + RH + 8;
    reg_cfg(g_hCfgClearBtn = make_button(hwnd, "Clear", IDC_CFG_CLEAR_BTN,
                                         cx, cy, 65, RH));
}

/* ================================================================
 * 窗口创建
 * ================================================================ */

static void create_controls(HWND hwnd)
{
    /* 创建 Tab Control */
    g_hTabCtrl = CreateWindowA(WC_TABCONTROLA, "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, CLIENT_W, CLIENT_H,
            hwnd, (HMENU)(LONG_PTR)IDC_TAB_CTRL, g_hInst, NULL);
    SendMessage(g_hTabCtrl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    /* 添加 Tab 页 */
    TCITEMA tie = {0};
    tie.mask = TCIF_TEXT;
    tie.pszText = (LPSTR)"Data";
    TabCtrl_InsertItem(g_hTabCtrl, 0, &tie);
    tie.pszText = (LPSTR)"Config";
    TabCtrl_InsertItem(g_hTabCtrl, 1, &tie);

    /* 获取 Tab 显示区域 */
    RECT pageRc = {0, 0, CLIENT_W, CLIENT_H};
    TabCtrl_AdjustRect(g_hTabCtrl, FALSE, &pageRc);

    /* 创建两个页面的控件 */
    create_data_page(hwnd, &pageRc);
    create_config_page(hwnd, &pageRc);

    /* 默认显示数据页 */
    switch_tab(0);
}

/* ================================================================
 * 窗口过程
 * ================================================================ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        update_connection_status();
        update_stats();
        log_append("LoRa Gateway Tool started");
        return 0;

    case WM_NOTIFY: {
        NMHDR *pnm = (NMHDR *)lParam;
        if (pnm->idFrom == IDC_TAB_CTRL && pnm->code == TCN_SELCHANGE) {
            switch_tab(TabCtrl_GetCurSel(g_hTabCtrl));
        }
        return 0;
    }

    case WM_SOCKET: {
        int event = WSAGETSELECTEVENT(lParam);
        int error = WSAGETSELECTERROR(lParam);

        switch (event) {
        case FD_CONNECT:
            if (error) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "Connect failed (error %d)", error);
                log_append(buf);
                closesocket(g_sock);
                g_sock = INVALID_SOCKET;
                g_connected = FALSE;
                update_connection_status();
            } else {
                g_connected = TRUE;
                g_rx_len = 0;
                update_connection_status();
                log_append("Connected to gateway");
            }
            break;

        case FD_READ:
            process_rx_data();
            break;

        case FD_CLOSE:
            log_append("Connection closed by remote");
            do_disconnect();
            break;
        }
        return 0;
    }

    case WM_UDP_LOG:
        if (lParam) {
            cfg_log_append((const char *)lParam);
            free((void *)lParam);
        }
        return 0;

    case WM_UDP_RX:
        if (lParam) {
            udp_rx_msg_t *rx = (udp_rx_msg_t *)lParam;
            udp_process_response(rx->from_ip, rx->data);
            free(rx);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        /* 数据页按钮 */
        case IDC_CONNECT_BTN:
            do_connect();
            break;
        case IDC_DISCONNECT_BTN:
            do_disconnect();
            break;
        case IDC_SEND_BTN:
            do_manual_send();
            break;
        case IDC_SEND_ACK_BTN:
            do_manual_ack();
            break;
        case IDC_CLEAR_LOG_BTN:
            g_log_len = 0;
            g_log_buf[0] = '\0';
            SetWindowTextA(g_hLogEdit, "");
            break;
        case IDC_AUTO_ACK_CHECK:
            g_auto_ack = (SendMessage(g_hAutoAckCheck, BM_GETCHECK, 0, 0)
                          == BST_CHECKED);
            break;

        /* 配置页按钮 */
        case IDC_CFG_SEARCH_BTN:
            do_cfg_search();
            break;
        case IDC_CFG_GETNET_BTN:
            do_cfg_get_net();
            break;
        case IDC_CFG_SEND_BTN:
            do_cfg_send();
            break;
        case IDC_CFG_CLEAR_BTN:
            g_cfg_log_len = 0;
            g_cfg_log_buf[0] = '\0';
            SetWindowTextA(g_hCfgLogEdit, "");
            break;
        case IDC_CFG_QUERY_VER:
            do_cfg_quick("AT+VER?\r\n");
            break;
        case IDC_CFG_QUERY_GWID:
            do_cfg_quick("AT+GWID?\r\n");
            break;
        case IDC_CFG_QUERY_CSQ:
            do_cfg_quick("AT+CSQ?\r\n");
            break;
        }
        return 0;

    case WM_DESTROY:
        do_disconnect();
        WSACleanup();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ================================================================
 * WinMain
 * ================================================================ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst,
                   LPSTR cmdLine, int cmdShow)
{
    (void)hPrevInst;
    (void)cmdLine;
    g_hInst = hInst;

    /* 初始化 Winsock */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        MessageBoxA(NULL, "WSAStartup failed", "Error", MB_OK);
        return 1;
    }

    /* 初始化 Common Controls (ListView + Tab) */
    INITCOMMONCONTROLSEX icc = {
        .dwSize = sizeof(icc),
        .dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES,
    };
    InitCommonControlsEx(&icc);

    /* 注册窗口类 */
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "LoRaGatewayTool";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExA(&wc);

    /* 创建字体 */
    g_hFont = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          FF_DONTCARE, "Segoe UI");
    g_hFontBold = CreateFontA(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FF_DONTCARE, "Segoe UI");
    g_hFontTitle = CreateFontA(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FF_DONTCARE, "Segoe UI");

    /* 计算精确窗口尺寸 (客户区 920x640) */
    RECT rc = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION |
                     WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_hwndMain = CreateWindowExA(0, "LoRaGatewayTool",
                                 "LoRa Gateway Tool v1.0",
                                 WS_OVERLAPPED | WS_CAPTION |
                                 WS_SYSMENU | WS_MINIMIZEBOX,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 rc.right - rc.left, rc.bottom - rc.top,
                                 NULL, NULL, hInst, NULL);

    ShowWindow(g_hwndMain, cmdShow);
    UpdateWindow(g_hwndMain);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DeleteObject(g_hFont);
    DeleteObject(g_hFontBold);
    DeleteObject(g_hFontTitle);
    return (int)msg.wParam;
}
