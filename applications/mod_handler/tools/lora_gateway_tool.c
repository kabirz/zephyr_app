/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoRa Gateway Host Tool — Win32 GUI
 *
 * 连接 USR-LG210-L 网关，接收/解析 LoRa 数据帧。
 * 支持遥测数据显示、原始数据日志、手动发送、历史记录。
 * 通过 UDP 广播 AT 指令配置 LoRa 模块。
 *
 * 协议 (TCP TX): [NID 4B][0xAA][0x55][NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE][\r\n]
 * 协议 (TCP RX): [0xAA][0x55][NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE][\r\n]
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lora_net.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")

/* ================================================================
 * 常量定义
 * ================================================================ */

#define LOG_BUF_MAX   65536
#define HISTORY_MAX   500

#define WM_SOCKET       (WM_USER + 1)
#define WM_UDP_LOG      (WM_USER + 2)   /* LPARAM = strdup'd string, 接收后 free() */
#define WM_UDP_RX       (WM_USER + 3)   /* LPARAM = udp_rx_msg_t*, 接收后 net_on_udp_rx free */
#define WM_TCP_RX       (WM_USER + 4)   /* LPARAM = tcp_rx_chunk_t*, 接收后 net_on_tcp_rx free */
#define WM_TCP_CLOSED   (WM_USER + 5)   /* 连接被远端关闭 */

#define CLIENT_W      1100
#define CLIENT_H      780

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

/* 网络 设置控件 ID */
#define IDC_CFG_DHCP_TEXT      1220
#define IDC_CFG_DHCP_QUERY     1221
#define IDC_CFG_DHCP_ON        1222
#define IDC_CFG_DHCP_OFF       1223
#define IDC_CFG_IP_SET_EDIT    1224
#define IDC_CFG_IP_SET_BTN     1225
#define IDC_CFG_IP_QUERY_BTN   1226
#define IDC_CFG_MASK_SET_EDIT  1227
#define IDC_CFG_MASK_SET_BTN   1228
#define IDC_CFG_MASK_QUERY_BTN 1229
#define IDC_CFG_GW_SET_EDIT    1230
#define IDC_CFG_GW_SET_BTN     1231
#define IDC_CFG_GW_QUERY_BTN   1232
#define IDC_CFG_OPTION_COMBO   1233
#define IDC_CFG_OPTION_SET     1234
#define IDC_CFG_OPTION_QUERY   1235
#define IDC_CFG_OPTION_TEXT    1236

/* LoRa 协议 控件 ID */
#define IDC_CFG_NWMODE_COMBO  1240
#define IDC_CFG_NWMODE_SET    1241
#define IDC_CFG_NWMODE_QUERY  1242
#define IDC_CFG_NWMODE_TEXT   1243
#define IDC_CFG_TTMODE_COMBO  1244
#define IDC_CFG_TTMODE_SET    1245
#define IDC_CFG_TTMODE_QUERY  1246
#define IDC_CFG_TTMODE_TEXT   1247
#define IDC_CFG_WMODE_COMBO   1248
#define IDC_CFG_WMODE_SET     1249
#define IDC_CFG_WMODE_QUERY   1250
#define IDC_CFG_WMODE_TEXT    1251
#define IDC_CFG_UPWID_TEXT    1252
#define IDC_CFG_UPWID_QUERY   1253
#define IDC_CFG_UPWID_ON      1254
#define IDC_CFG_UPWID_OFF     1255

/* LoRa 通道参数控件 ID */
#define IDC_CFG_CH_COMBO      1256
#define IDC_CFG_CH_EDIT       1257
#define IDC_CFG_CH_SET        1258
#define IDC_CFG_CH_QUERY      1259
#define IDC_CFG_SPD_EDIT      1260
#define IDC_CFG_SPD_SET       1261
#define IDC_CFG_SPD_QUERY     1262
#define IDC_CFG_PWR_EDIT      1263
#define IDC_CFG_PWR_SET       1264
#define IDC_CFG_PWR_QUERY     1265

/* ================================================================
 * UI 全局状态
 * ================================================================ */

static HINSTANCE g_hInst;
static HWND g_hwndMain;

/* 网络上下文 */
static net_ctx_t *g_net_ctx;

/* Tab 控件 */
static HWND g_hTabCtrl;

/* 数据页控件 */
static HWND g_hIpEdit, g_hPortEdit, g_hConnectBtn, g_hDisconnectBtn;
static HWND g_hNidEdit;
static HWND g_hSendEdit, g_hSendBtn, g_hClearLogBtn;
static HWND g_hStatusText;
static HWND g_hXText, g_hYText, g_hBtnText;
static HWND g_hRxCount, g_hTxCount, g_hErrCount;
static HWND g_hLogEdit;
static HWND g_hHistoryList;

/* 配置页控件 */
static HWND g_hCfgCmdEdit, g_hCfgSendBtn, g_hCfgLogEdit, g_hCfgClearBtn;
static HWND g_hCfgQueryVer, g_hCfgQueryGwid, g_hCfgQueryCsq;
static HWND g_hCfgSearchBtn, g_hCfgGetNetBtn;
static HWND g_hCfgMacText, g_hCfgDevText, g_hCfgSwText;
static HWND g_hCfgGwidText, g_hCfgCsqText;

/* 网络 设置控件 */
static HWND g_hCfgDhcpText, g_hCfgDhcpQuery, g_hCfgDhcpOn, g_hCfgDhcpOff;
static HWND g_hCfgIpSetEdit, g_hCfgIpSetBtn, g_hCfgIpQueryBtn;
static HWND g_hCfgMaskSetEdit, g_hCfgMaskSetBtn, g_hCfgMaskQueryBtn;
static HWND g_hCfgGwSetEdit, g_hCfgGwSetBtn, g_hCfgGwQueryBtn;
static HWND g_hCfgOptionCombo, g_hCfgOptionSet, g_hCfgOptionQuery;
static HWND g_hCfgOptionText;

/* LoRa 协议控件 */
static HWND g_hCfgNwmodeCombo, g_hCfgNwmodeSet, g_hCfgNwmodeQuery;
static HWND g_hCfgTtmodeCombo, g_hCfgTtmodeSet, g_hCfgTtmodeQuery;
static HWND g_hCfgWmodeCombo, g_hCfgWmodeSet, g_hCfgWmodeQuery;
static HWND g_hCfgUpwidText, g_hCfgUpwidQuery, g_hCfgUpwidOn, g_hCfgUpwidOff;

/* LoRa 通道参数控件 */
static HWND g_hCfgChCombo, g_hCfgChEdit, g_hCfgChSet, g_hCfgChQuery;
static HWND g_hCfgSpdEdit, g_hCfgSpdSet, g_hCfgSpdQuery;
static HWND g_hCfgPwrEdit, g_hCfgPwrSet, g_hCfgPwrQuery;

/* 页面控件数组 (用于显示/隐藏) — 见 MAX_PAGE_CTLS 定义 */

/* 日志缓冲区 */
static char g_log_buf[LOG_BUF_MAX];
static int g_log_len = 0;

static char g_cfg_log_buf[LOG_BUF_MAX];
static int g_cfg_log_len = 0;

/* 字体 */
static HFONT g_hFont;
static HFONT g_hFontBold;
static HFONT g_hFontTitle;

/* ================================================================
 * 回调实现 — 供 lora_net.c 调用的 UI 更新函数
 * ================================================================ */

static void cb_update_stats(void *ud)
{
    (void)ud;
    char buf[32];
    snprintf(buf, sizeof(buf), "RX: %d", g_rx_count);
    SetWindowTextA(g_hRxCount, buf);
    snprintf(buf, sizeof(buf), "TX: %d", g_tx_count);
    SetWindowTextA(g_hTxCount, buf);
    snprintf(buf, sizeof(buf), "ERR: %d", g_err_count);
    SetWindowTextA(g_hErrCount, buf);
}

static void cb_update_telemetry(void *ud)
{
    (void)ud;
    char buf[64];
    snprintf(buf, sizeof(buf), "X Angle: %.1f deg", g_last_x / 10.0);
    SetWindowTextA(g_hXText, buf);
    snprintf(buf, sizeof(buf), "Y Angle: %.1f deg", g_last_y / 10.0);
    SetWindowTextA(g_hYText, buf);
    snprintf(buf, sizeof(buf), "Button: %s",
             g_last_btn ? "Released" : "Pressed");
    SetWindowTextA(g_hBtnText, buf);
}

static void cb_add_history_entry(void *ud, uint32_t nid, const char *type,
                                 const uint8_t *data, uint16_t data_len)
{
    (void)ud;

    char timebuf[16];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

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

static void cb_log_append(void *ud, const char *text)
{
    (void)ud;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timebuf[16];
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

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

static void cb_log_hex(void *ud, const char *prefix, const uint8_t *data, int len)
{
    (void)ud;
    char hex[1024];
    int offset = snprintf(hex, sizeof(hex), "%s (%d bytes): ", prefix, len);
    for (int i = 0; i < len && offset < (int)sizeof(hex) - 4; i++) {
        offset += snprintf(hex + offset, sizeof(hex) - offset, "%02X ", data[i]);
    }
    cb_log_append(ud, hex);
}

static void cb_cfg_log_append(void *ud, const char *text)
{
    (void)ud;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timebuf[16];
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

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

static void cb_set_nid_text(void *ud, const char *nid_hex)
{
    (void)ud;
    SetWindowTextA(g_hNidEdit, nid_hex);
}

static void cb_set_status_text(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hStatusText, text);
}

static void cb_set_connect_enabled(void *ud, int enabled)
{
    (void)ud;
    EnableWindow(g_hConnectBtn, enabled);
}

static void cb_set_disconnect_enabled(void *ud, int enabled)
{
    (void)ud;
    EnableWindow(g_hDisconnectBtn, enabled);
}

static void cb_set_cfg_device_mac(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hCfgMacText, text);
}

static void cb_set_cfg_device_name(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hCfgDevText, text);
}

static void cb_set_cfg_device_sw(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hCfgSwText, text);
}

static void cb_set_cfg_ip(void *ud, const char *text)
{
    (void)ud;
    /* 合并后直接更新 edit 控件 (传入 "IP: x.x.x.x" 格式，提取地址部分) */
    const char *v = text;
    if (strncmp(text, "IP: ", 4) == 0) v = text + 4;
    SetWindowTextA(g_hCfgIpSetEdit, v);
}

static void cb_set_cfg_sm(void *ud, const char *text)
{
    (void)ud;
    const char *v = text;
    if (strncmp(text, "Mask: ", 6) == 0) v = text + 6;
    SetWindowTextA(g_hCfgMaskSetEdit, v);
}

static void cb_set_cfg_gw(void *ud, const char *text)
{
    (void)ud;
    const char *v = text;
    if (strncmp(text, "GW: ", 4) == 0) v = text + 4;
    SetWindowTextA(g_hCfgGwSetEdit, v);
}

static void cb_set_cfg_gwid(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hCfgGwidText, text);
}

static void cb_set_cfg_csq(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hCfgCsqText, text);
}

static void cb_set_cfg_cmd_edit(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hCfgCmdEdit, text);
}

static void cb_set_cfg_dhcp(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hCfgDhcpText, text);
}

/* cb_set_cfg_ip/sm/gw 已合并更新 edit 控件, 无需 _edit 变体 */

static void cb_set_cfg_option(void *ud, const char *text)
{
    (void)ud;
    SetWindowTextA(g_hCfgOptionText, text);
}

static void cb_set_cfg_option_edit(void *ud, const char *text)
{
    (void)ud;
    int mode = atoi(text);
    if (mode >= 0 && mode <= 4)
        ComboBox_SetCurSel(g_hCfgOptionCombo, mode);
}

static void cb_set_cfg_nwmode(void *ud, const char *text)
{
    (void)ud;
    int mode = atoi(text);
    if (mode < 0 || mode > 1) return;
    ComboBox_SetCurSel(g_hCfgNwmodeCombo, mode);
}

static void cb_set_cfg_ttmode(void *ud, const char *text)
{
    (void)ud;
    int mode = atoi(text);
    if (mode < 0 || mode > 1) return;
    ComboBox_SetCurSel(g_hCfgTtmodeCombo, mode);
}

static void cb_set_cfg_wmode(void *ud, const char *text)
{
    (void)ud;
    int mode = atoi(text);
    if (mode < 0 || mode > 2) return;
    ComboBox_SetCurSel(g_hCfgWmodeCombo, mode);
}

static void cb_set_cfg_upwid(void *ud, const char *text)
{
    (void)ud;
    char txt[64];
    snprintf(txt, sizeof(txt), "UPWID: %s", text);
    SetWindowTextA(g_hCfgUpwidText, txt);
}

static void cb_set_cfg_ch(void *ud, const char *text)
{
    (void)ud;
    int val = atoi(text);
    if (val >= 4100 && val <= 5100 && (val % 100) == 0)
        ComboBox_SetCurSel(g_hCfgChEdit, (val - 4100) / 100);
}

static void cb_set_cfg_spd(void *ud, const char *text)
{
    (void)ud;
    int val = atoi(text);
    if (val >= 4 && val <= 11)
        ComboBox_SetCurSel(g_hCfgSpdEdit, val - 4);
}

static void cb_set_cfg_pwr(void *ud, const char *text)
{
    (void)ud;
    int val = atoi(text);
    if (val >= 24 && val <= 30)
        ComboBox_SetCurSel(g_hCfgPwrEdit, val - 24);
}

static void cb_show_error(void *ud, const char *title, const char *message)
{
    (void)ud;
    MessageBoxA(g_hwndMain, message, title, MB_OK);
}

static void cb_update_connection_status(void *ud)
{
    (void)ud;
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

/* ================================================================
 * 回调表
 * ================================================================ */

static const net_callbacks_t g_callbacks = {
    .update_stats           = cb_update_stats,
    .update_telemetry       = cb_update_telemetry,
    .add_history_entry      = cb_add_history_entry,
    .log_append             = cb_log_append,
    .log_hex                = cb_log_hex,
    .cfg_log_append         = cb_cfg_log_append,
    .set_nid_text           = cb_set_nid_text,
    .set_status_text        = cb_set_status_text,
    .set_connect_enabled    = cb_set_connect_enabled,
    .set_disconnect_enabled = cb_set_disconnect_enabled,
    .set_cfg_device_mac     = cb_set_cfg_device_mac,
    .set_cfg_device_name    = cb_set_cfg_device_name,
    .set_cfg_device_sw      = cb_set_cfg_device_sw,
    .set_cfg_ip             = cb_set_cfg_ip,
    .set_cfg_sm             = cb_set_cfg_sm,
    .set_cfg_gw             = cb_set_cfg_gw,
    .set_cfg_gwid           = cb_set_cfg_gwid,
    .set_cfg_csq            = cb_set_cfg_csq,
    .set_cfg_cmd_edit       = cb_set_cfg_cmd_edit,
    .set_cfg_dhcp           = cb_set_cfg_dhcp,
    .set_cfg_ip_edit        = cb_set_cfg_ip,
    .set_cfg_sm_edit        = cb_set_cfg_sm,
    .set_cfg_gw_edit        = cb_set_cfg_gw,
    .set_cfg_option         = cb_set_cfg_option,
    .set_cfg_option_edit    = cb_set_cfg_option_edit,
    .set_cfg_nwmode         = cb_set_cfg_nwmode,
    .set_cfg_ttmode         = cb_set_cfg_ttmode,
    .set_cfg_wmode          = cb_set_cfg_wmode,
    .set_cfg_upwid          = cb_set_cfg_upwid,
    .set_cfg_ch             = cb_set_cfg_ch,
    .set_cfg_spd            = cb_set_cfg_spd,
    .set_cfg_pwr            = cb_set_cfg_pwr,
    .show_error             = cb_show_error,
    .update_connection_status = cb_update_connection_status,
};

/* ================================================================
 * 页面控件注册
 * ================================================================ */

#define MAX_PAGE_CTLS 128

static HWND g_dataPage[MAX_PAGE_CTLS];
static int g_nDataPage = 0;
static HWND g_cfgPage[MAX_PAGE_CTLS];
static int g_nCfgPage = 0;

/* ... */

static void reg_data(HWND h)
{
    if (h && g_nDataPage < MAX_PAGE_CTLS) g_dataPage[g_nDataPage++] = h;
}

static void reg_cfg(HWND h)
{
    if (h && g_nCfgPage < MAX_PAGE_CTLS) g_cfgPage[g_nCfgPage++] = h;
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
    net_send_data_frame(g_net_ctx, nid, data, (uint16_t)len);
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

    /* 第二行: NID */
    cy += RH + 4;
    reg_data(make_static(hwnd, "NID:", cx, cy + 3, 28, RH, g_hFont));
    reg_data(g_hNidEdit = make_edit(hwnd, "--------", IDC_NID_EDIT,
                                    cx + 32, cy, 90, RH));
    SendMessage(g_hNidEdit, EM_SETREADONLY, TRUE, 0);

    y += conn_h + GAP;

    /* ── 2. Telemetry + Raw Log ── */
    int mid_h = RH * 4 + 30;
    int telem_w = 320;

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
    reg_data(g_hSendEdit = make_edit(hwnd, "", IDC_SEND_EDIT, sx + 34, sy, 530, RH));
    reg_data(g_hSendBtn = make_button(hwnd, "Send", IDC_SEND_BTN, sx + 570, sy, 65, RH));
    reg_data(g_hClearLogBtn = make_button(hwnd, "Clear Log", IDC_CLEAR_LOG_BTN, sx + 641, sy, 80, RH));

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

    col.cx = 140;                        col.pszText = "Time";
    ListView_InsertColumn(g_hHistoryList, 0, &col);
    col.cx = 110;                        col.pszText = "NID";
    ListView_InsertColumn(g_hHistoryList, 1, &col);
    col.cx = 90;                         col.pszText = "Type";
    ListView_InsertColumn(g_hHistoryList, 2, &col);
    col.cx = lv_w - 140 - 110 - 90;     col.pszText = "Data";
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
                                          cx, cy, 140, RH));
    reg_cfg(g_hCfgGetNetBtn = make_button(hwnd, "Get Network", IDC_CFG_GETNET_BTN,
                                          cx + 148, cy, 140, RH));
    reg_cfg(g_hCfgQueryGwid = make_button(hwnd, "Query GWID", IDC_CFG_QUERY_GWID,
                                           cx + 296, cy, 140, RH));
    reg_cfg(g_hCfgQueryCsq = make_button(hwnd, "Query Signal", IDC_CFG_QUERY_CSQ,
                                          cx + 444, cy, 140, RH));

    cy += RH + 4;
    reg_cfg(g_hCfgMacText = make_static(hwnd, "MAC: --", cx, cy, 180, RH, g_hFont));
    reg_cfg(g_hCfgDevText = make_static(hwnd, "Device: --", cx + 184, cy, 200, RH, g_hFont));
    reg_cfg(g_hCfgSwText = make_static(hwnd, "SW: --", cx + 388, cy, 180, RH, g_hFont));
    reg_cfg(g_hCfgGwidText = make_static(hwnd, "GWID: --", cx + 572, cy, 180, RH, g_hFont));
    reg_cfg(g_hCfgCsqText = make_static(hwnd, "Signal: --", cx + 756, cy, 180, RH, g_hFont));

    y += dev_h + GAP;

    /* ── 2. Network ── */
    int net_h = RH * 3 + 34;
    reg_cfg(make_groupbox(hwnd, " Network ", PX, y, PW, net_h));

    cy = y + 18;

    /* Row 1: DHCP + Option */
    reg_cfg(make_static(hwnd, "DHCP:", cx, cy + 3, 44, RH, g_hFont));
    reg_cfg(g_hCfgDhcpText = make_static(hwnd, "--", cx + 48, cy + 3, 70, RH, g_hFontBold));
    reg_cfg(g_hCfgDhcpQuery = make_button(hwnd, "Query", IDC_CFG_DHCP_QUERY, cx + 124, cy, 55, RH));
    reg_cfg(g_hCfgDhcpOn = make_button(hwnd, "ON", IDC_CFG_DHCP_ON, cx + 185, cy, 50, RH));
    reg_cfg(g_hCfgDhcpOff = make_button(hwnd, "OFF", IDC_CFG_DHCP_OFF, cx + 241, cy, 53, RH));

    int rx = cx + PW / 2 + 10;
    reg_cfg(make_static(hwnd, "Mode:", rx, cy + 3, 36, RH, g_hFont));
    reg_cfg(g_hCfgOptionCombo = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            rx + 40, cy, 150, 200,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_OPTION_COMBO, g_hInst, NULL));
    SendMessage(g_hCfgOptionCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ComboBox_AddString(g_hCfgOptionCombo, "socket");
    ComboBox_AddString(g_hCfgOptionCombo, "serial");
    ComboBox_AddString(g_hCfgOptionCombo, "mqtt");
    ComboBox_AddString(g_hCfgOptionCombo, "ali_cloud");
    ComboBox_AddString(g_hCfgOptionCombo, "usr_cloud");
    ComboBox_SetCurSel(g_hCfgOptionCombo, 0);
    reg_cfg(g_hCfgOptionSet = make_button(hwnd, "Set", IDC_CFG_OPTION_SET, rx + 196, cy, 50, RH));
    reg_cfg(g_hCfgOptionQuery = make_button(hwnd, "Query", IDC_CFG_OPTION_QUERY, rx + 252, cy, 55, RH));

    cy += RH + 4;

    /* Row 2: IP + Mask */
    reg_cfg(make_static(hwnd, "IP:", cx, cy + 3, 20, RH, g_hFont));
    reg_cfg(g_hCfgIpSetEdit = make_edit(hwnd, "", IDC_CFG_IP_SET_EDIT, cx + 24, cy, 260, RH));
    reg_cfg(g_hCfgIpSetBtn = make_button(hwnd, "Set", IDC_CFG_IP_SET_BTN, cx + 290, cy, 50, RH));
    reg_cfg(g_hCfgIpQueryBtn = make_button(hwnd, "Query", IDC_CFG_IP_QUERY_BTN, cx + 346, cy, 55, RH));

    reg_cfg(make_static(hwnd, "Mask:", rx, cy + 3, 40, RH, g_hFont));
    reg_cfg(g_hCfgMaskSetEdit = make_edit(hwnd, "", IDC_CFG_MASK_SET_EDIT, rx + 44, cy, 250, RH));
    reg_cfg(g_hCfgMaskSetBtn = make_button(hwnd, "Set", IDC_CFG_MASK_SET_BTN, rx + 300, cy, 50, RH));
    reg_cfg(g_hCfgMaskQueryBtn = make_button(hwnd, "Query", IDC_CFG_MASK_QUERY_BTN, rx + 356, cy, 55, RH));

    cy += RH + 4;

    /* Row 3: GW */
    reg_cfg(make_static(hwnd, "GW:", cx, cy + 3, 24, RH, g_hFont));
    reg_cfg(g_hCfgGwSetEdit = make_edit(hwnd, "", IDC_CFG_GW_SET_EDIT, cx + 28, cy, 256, RH));
    reg_cfg(g_hCfgGwSetBtn = make_button(hwnd, "Set", IDC_CFG_GW_SET_BTN, cx + 290, cy, 50, RH));
    reg_cfg(g_hCfgGwQueryBtn = make_button(hwnd, "Query", IDC_CFG_GW_QUERY_BTN, cx + 346, cy, 55, RH));

    y += net_h + GAP;

    /* ── 3. LoRa Protocol ── */
    int lora_h = RH * 3 + 38;
    reg_cfg(make_groupbox(hwnd, " LoRa Protocol ", PX, y, PW, lora_h));

    cy = y + 18;

    /* Row 1: NWMODE + TTMODE */
    reg_cfg(make_static(hwnd, "NWMODE:", cx, cy + 3, 70, RH, g_hFont));
    reg_cfg(g_hCfgNwmodeCombo = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + 74, cy, 150, 200,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_NWMODE_COMBO, g_hInst, NULL));
    SendMessage(g_hCfgNwmodeCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ComboBox_AddString(g_hCfgNwmodeCombo, "透传 (默认)");
    ComboBox_AddString(g_hCfgNwmodeCombo, "组网");
    ComboBox_SetCurSel(g_hCfgNwmodeCombo, 0);
    reg_cfg(g_hCfgNwmodeSet = make_button(hwnd, "Set", IDC_CFG_NWMODE_SET, cx + 230, cy, 55, RH));
    reg_cfg(g_hCfgNwmodeQuery = make_button(hwnd, "Query", IDC_CFG_NWMODE_QUERY, cx + 291, cy, 60, RH));

    rx = cx + PW / 2 + 10;
    reg_cfg(make_static(hwnd, "TTMODE:", rx, cy + 3, 70, RH, g_hFont));
    reg_cfg(g_hCfgTtmodeCombo = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            rx + 74, cy, 150, 200,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_TTMODE_COMBO, g_hInst, NULL));
    SendMessage(g_hCfgTtmodeCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ComboBox_AddString(g_hCfgTtmodeCombo, "广播透传 (默认)");
    ComboBox_AddString(g_hCfgTtmodeCombo, "指定节点");
    ComboBox_SetCurSel(g_hCfgTtmodeCombo, 0);
    reg_cfg(g_hCfgTtmodeSet = make_button(hwnd, "Set", IDC_CFG_TTMODE_SET, rx + 230, cy, 55, RH));
    reg_cfg(g_hCfgTtmodeQuery = make_button(hwnd, "Query", IDC_CFG_TTMODE_QUERY, rx + 291, cy, 60, RH));

    cy += RH + 4;

    /* Row 2: WMODE + UPWID */
    reg_cfg(make_static(hwnd, "WMODE:", cx, cy + 3, 70, RH, g_hFont));
    reg_cfg(g_hCfgWmodeCombo = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            cx + 74, cy, 150, 200,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_WMODE_COMBO, g_hInst, NULL));
    SendMessage(g_hCfgWmodeCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ComboBox_AddString(g_hCfgWmodeCombo, "广播透传 (默认)");
    ComboBox_AddString(g_hCfgWmodeCombo, "指定节点");
    ComboBox_AddString(g_hCfgWmodeCombo, "主动上报");
    ComboBox_SetCurSel(g_hCfgWmodeCombo, 0);
    reg_cfg(g_hCfgWmodeSet = make_button(hwnd, "Set", IDC_CFG_WMODE_SET, cx + 230, cy, 55, RH));
    reg_cfg(g_hCfgWmodeQuery = make_button(hwnd, "Query", IDC_CFG_WMODE_QUERY, cx + 291, cy, 60, RH));

    reg_cfg(g_hCfgUpwidText = make_static(hwnd, "UPWID: --", rx, cy + 3, 130, RH, g_hFont));
    reg_cfg(g_hCfgUpwidQuery = make_button(hwnd, "Query", IDC_CFG_UPWID_QUERY, rx + 138, cy, 60, RH));
    reg_cfg(g_hCfgUpwidOn = make_button(hwnd, "Set ON", IDC_CFG_UPWID_ON, rx + 206, cy, 70, RH));
    reg_cfg(g_hCfgUpwidOff = make_button(hwnd, "Set OFF", IDC_CFG_UPWID_OFF, rx + 284, cy, 70, RH));

    cy += RH + 4;

    /* Row 3: CH# + CH + SPD + PWR */
    int ax = cx;
    reg_cfg(make_static(hwnd, "CH#:", ax, cy + 3, 38, RH, g_hFont));
    reg_cfg(g_hCfgChCombo = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ax + 40, cy, 44, 200,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_CH_COMBO, g_hInst, NULL));
    SendMessage(g_hCfgChCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ComboBox_AddString(g_hCfgChCombo, "1");
    ComboBox_AddString(g_hCfgChCombo, "2");
    ComboBox_SetCurSel(g_hCfgChCombo, 0);

    ax += 92;
    reg_cfg(make_static(hwnd, "CH:", ax, cy + 3, 38, RH, g_hFont));
    reg_cfg(g_hCfgChEdit = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ax + 42, cy, 68, 200,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_CH_EDIT, g_hInst, NULL));
    SendMessage(g_hCfgChEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    for (int i = 4100; i <= 5100; i += 100) {
        char s[8]; snprintf(s, sizeof(s), "%d", i);
        ComboBox_AddString(g_hCfgChEdit, s);
    }
    ComboBox_SetCurSel(g_hCfgChEdit, 6); /* 默认 4700 */
    reg_cfg(g_hCfgChSet = make_button(hwnd, "Set", IDC_CFG_CH_SET, ax + 114, cy, 45, RH));
    reg_cfg(g_hCfgChQuery = make_button(hwnd, "Query", IDC_CFG_CH_QUERY, ax + 163, cy, 50, RH));

    ax += 222;
    reg_cfg(make_static(hwnd, "SPD:", ax, cy + 3, 38, RH, g_hFont));
    reg_cfg(g_hCfgSpdEdit = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ax + 42, cy, 52, 200,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_SPD_EDIT, g_hInst, NULL));
    SendMessage(g_hCfgSpdEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    for (int i = 4; i <= 11; i++) {
        char s[8]; snprintf(s, sizeof(s), "%d", i);
        ComboBox_AddString(g_hCfgSpdEdit, s);
    }
    ComboBox_SetCurSel(g_hCfgSpdEdit, 3); /* 默认 7 */
    reg_cfg(g_hCfgSpdSet = make_button(hwnd, "Set", IDC_CFG_SPD_SET, ax + 98, cy, 45, RH));
    reg_cfg(g_hCfgSpdQuery = make_button(hwnd, "Query", IDC_CFG_SPD_QUERY, ax + 147, cy, 50, RH));

    ax += 208;
    reg_cfg(make_static(hwnd, "PWR:", ax, cy + 3, 38, RH, g_hFont));
    reg_cfg(g_hCfgPwrEdit = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ax + 42, cy, 52, 200,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_PWR_EDIT, g_hInst, NULL));
    SendMessage(g_hCfgPwrEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    for (int i = 24; i <= 30; i++) {
        char s[8]; snprintf(s, sizeof(s), "%d", i);
        ComboBox_AddString(g_hCfgPwrEdit, s);
    }
    ComboBox_SetCurSel(g_hCfgPwrEdit, 6); /* 默认 30 */
    reg_cfg(g_hCfgPwrSet = make_button(hwnd, "Set", IDC_CFG_PWR_SET, ax + 90, cy, 45, RH));
    reg_cfg(g_hCfgPwrQuery = make_button(hwnd, "Query", IDC_CFG_PWR_QUERY, ax + 139, cy, 50, RH));

    y += lora_h + GAP;

    /* ── 3. AT Command ── */
    int cmd_h = RH + 30;
    reg_cfg(make_groupbox(hwnd, " AT Command (UDP :1566) ", PX, y, PW, cmd_h));

    cy = y + 18;
    reg_cfg(make_static(hwnd, "CMD:", cx, cy + 3, 32, RH, g_hFont));
    reg_cfg(g_hCfgCmdEdit = make_edit(hwnd, "AT+VER?\r\n", IDC_CFG_CMD_EDIT,
                                      cx + 36, cy, PW - 140, RH));
    reg_cfg(g_hCfgSendBtn = make_button(hwnd, "Send", IDC_CFG_SEND_BTN,
                                        cx + PW - 96, cy, 60, RH));

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
    int log_h = pageRc->bottom - y - 4;
    if (log_h < 60) log_h = 60;
    reg_cfg(make_groupbox(hwnd, " Response ", PX, y, PW, log_h));

    reg_cfg(g_hCfgClearBtn = make_button(hwnd, "Clear", IDC_CFG_CLEAR_BTN,
                                         PX + PW - 80, y + 2, 65, 22));

    reg_cfg(g_hCfgLogEdit = CreateWindowA("EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            PX + 10, y + 20, PW - 20, log_h - 24,
            hwnd, (HMENU)(LONG_PTR)IDC_CFG_LOG_EDIT, g_hInst, NULL));
    SendMessage(g_hCfgLogEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
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
        cb_update_connection_status(NULL);
        cb_update_stats(NULL);
        cb_log_append(NULL, "LoRa Gateway Tool started");
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
        net_on_socket_event(g_net_ctx, event, error);
        return 0;
    }

    case WM_UDP_LOG:
        if (lParam) {
            net_on_udp_log(g_net_ctx, (const char *)lParam);
            free((void *)lParam);
        }
        return 0;

    case WM_UDP_RX:
        if (lParam) {
            net_on_udp_rx(g_net_ctx, (udp_rx_msg_t *)lParam);
        }
        return 0;

    case WM_TCP_RX:
        if (lParam) {
            net_on_tcp_rx(g_net_ctx, (tcp_rx_chunk_t *)lParam);
        }
        return 0;

    case WM_TCP_CLOSED:
        net_disconnect(g_net_ctx);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        /* 数据页按钮 */
        case IDC_CONNECT_BTN: {
            char ip[64], port_str[16];
            GetWindowTextA(g_hIpEdit, ip, sizeof(ip));
            GetWindowTextA(g_hPortEdit, port_str, sizeof(port_str));
            net_connect(g_net_ctx, ip, atoi(port_str));
            break;
        }
        case IDC_DISCONNECT_BTN:
            net_disconnect(g_net_ctx);
            break;
        case IDC_SEND_BTN:
            do_manual_send();
            break;
        case IDC_CLEAR_LOG_BTN:
            g_log_len = 0;
            g_log_buf[0] = '\0';
            SetWindowTextA(g_hLogEdit, "");
            break;
        /* 配置页按钮 */
        case IDC_CFG_SEARCH_BTN:
            net_cfg_search(g_net_ctx);
            break;
        case IDC_CFG_GETNET_BTN:
            net_cfg_get_net(g_net_ctx);
            break;
        case IDC_CFG_SEND_BTN: {
            char cmd[256];
            GetWindowTextA(g_hCfgCmdEdit, cmd, sizeof(cmd));
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_CLEAR_BTN:
            g_cfg_log_len = 0;
            g_cfg_log_buf[0] = '\0';
            SetWindowTextA(g_hCfgLogEdit, "");
            break;
        case IDC_CFG_QUERY_VER:
            net_cfg_quick(g_net_ctx, "AT+VER?\r\n");
            break;
        case IDC_CFG_QUERY_GWID:
            net_cfg_quick(g_net_ctx, "AT+GWID?\r\n");
            break;
        case IDC_CFG_QUERY_CSQ:
            net_cfg_quick(g_net_ctx, "AT+CSQ?\r\n");
            break;

        /* DHCP */
        case IDC_CFG_DHCP_QUERY:
            net_cfg_send(g_net_ctx, "AT+DHCP?\r\n");
            break;
        case IDC_CFG_DHCP_ON:
            net_cfg_send(g_net_ctx, "AT+DHCP=ON\r\n");
            break;
        case IDC_CFG_DHCP_OFF:
            net_cfg_send(g_net_ctx, "AT+DHCP=OFF\r\n");
            break;

        /* IP */
        case IDC_CFG_IP_SET_BTN: {
            char ip[64];
            GetWindowTextA(g_hCfgIpSetEdit, ip, sizeof(ip));
            if (strlen(ip) == 0) {
                MessageBoxA(g_hwndMain, "Please enter IP address", "Error", MB_OK);
                break;
            }
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "AT+GWIP=%s\r\n", ip);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_IP_QUERY_BTN:
            net_cfg_send(g_net_ctx, "AT+GWIP?\r\n");
            break;

        /* Mask */
        case IDC_CFG_MASK_SET_BTN: {
            char mask[64];
            GetWindowTextA(g_hCfgMaskSetEdit, mask, sizeof(mask));
            if (strlen(mask) == 0) {
                MessageBoxA(g_hwndMain, "Please enter subnet mask", "Error", MB_OK);
                break;
            }
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "AT+MASK=%s\r\n", mask);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_MASK_QUERY_BTN:
            net_cfg_send(g_net_ctx, "AT+MASK?\r\n");
            break;

        /* GW */
        case IDC_CFG_GW_SET_BTN: {
            char gw[64];
            GetWindowTextA(g_hCfgGwSetEdit, gw, sizeof(gw));
            if (strlen(gw) == 0) {
                MessageBoxA(g_hwndMain, "Please enter gateway IP", "Error", MB_OK);
                break;
            }
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "AT+GW=%s\r\n", gw);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_GW_QUERY_BTN:
            net_cfg_send(g_net_ctx, "AT+GW?\r\n");
            break;

        /* Option */
        case IDC_CFG_OPTION_SET: {
            int sel = (int)SendMessage(g_hCfgOptionCombo, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) break;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+OPTION=%d\r\n", sel);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_OPTION_QUERY:
            net_cfg_send(g_net_ctx, "AT+OPTION?\r\n");
            break;

        /* NWMODE */
        case IDC_CFG_NWMODE_SET: {
            int sel = (int)SendMessage(g_hCfgNwmodeCombo, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) break;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+NWMODE=%d\r\n", sel);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_NWMODE_QUERY:
            net_cfg_send(g_net_ctx, "AT+NWMODE?\r\n");
            break;

        /* TTMODE */
        case IDC_CFG_TTMODE_SET: {
            int sel = (int)SendMessage(g_hCfgTtmodeCombo, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) break;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+TTMODE=%d\r\n", sel);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_TTMODE_QUERY:
            net_cfg_send(g_net_ctx, "AT+TTMODE?\r\n");
            break;

        /* WMODE */
        case IDC_CFG_WMODE_SET: {
            int sel = (int)SendMessage(g_hCfgWmodeCombo, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) break;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+WMODE=%d\r\n", sel);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_WMODE_QUERY:
            net_cfg_send(g_net_ctx, "AT+WMODE?\r\n");
            break;

        /* UPWID */
        case IDC_CFG_UPWID_QUERY:
            net_cfg_send(g_net_ctx, "AT+UPWID?\r\n");
            break;
        case IDC_CFG_UPWID_ON:
            net_cfg_send(g_net_ctx, "AT+UPWID=ON\r\n");
            break;
        case IDC_CFG_UPWID_OFF:
            net_cfg_send(g_net_ctx, "AT+UPWID=OFF\r\n");
            break;

        /* CH / SPD / PWR — 共享通道号 */
        case IDC_CFG_CH_SET: {
            char v[16];
            GetWindowTextA(g_hCfgChEdit, v, sizeof(v));
            if (strlen(v) == 0) break;
            int ch = (int)SendMessage(g_hCfgChCombo, CB_GETCURSEL, 0, 0) + 1;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+CH%d=%s\r\n", ch, v);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_CH_QUERY: {
            int ch = (int)SendMessage(g_hCfgChCombo, CB_GETCURSEL, 0, 0) + 1;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+CH%d?\r\n", ch);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_SPD_SET: {
            char v[16];
            GetWindowTextA(g_hCfgSpdEdit, v, sizeof(v));
            if (strlen(v) == 0) break;
            int ch = (int)SendMessage(g_hCfgChCombo, CB_GETCURSEL, 0, 0) + 1;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+SPD%d=%s\r\n", ch, v);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_SPD_QUERY: {
            int ch = (int)SendMessage(g_hCfgChCombo, CB_GETCURSEL, 0, 0) + 1;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+SPD%d?\r\n", ch);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_PWR_SET: {
            char v[16];
            GetWindowTextA(g_hCfgPwrEdit, v, sizeof(v));
            if (strlen(v) == 0) break;
            int ch = (int)SendMessage(g_hCfgChCombo, CB_GETCURSEL, 0, 0) + 1;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+PWR%d=%s\r\n", ch, v);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        case IDC_CFG_PWR_QUERY: {
            int ch = (int)SendMessage(g_hCfgChCombo, CB_GETCURSEL, 0, 0) + 1;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+PWR%d?\r\n", ch);
            net_cfg_send(g_net_ctx, cmd);
            break;
        }
        }
        return 0;

    case WM_DESTROY:
        net_cleanup(g_net_ctx);
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
    wc.hIcon   = LoadIcon(hInst, MAKEINTRESOURCE(2));
    wc.hIconSm = (HICON)LoadImage(hInst, MAKEINTRESOURCE(2),
                                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
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

    /* 初始化网络层 */
    g_net_ctx = net_init(g_hwndMain, &g_callbacks, NULL);
    if (!g_net_ctx) {
        MessageBoxA(NULL, "Failed to initialize network layer", "Error", MB_OK);
        return 1;
    }

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
