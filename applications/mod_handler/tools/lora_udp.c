/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_udp.c — UDP 设备发现与 LoRa 模块配置
 *
 * USR-LG210-L 网关 UDP 广播发现、网络参数获取、AT 指令透传。
 * 查询指令 (?) 用 GETPARA, 设置指令 (=) 用 SETPARA。
 * 使用独立线程 + 阻塞 socket + 5s 超时，与 Python 脚本一致。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lora_net.h"
#include "cJSON.h"

#pragma comment(lib, "iphlpapi.lib")

/* ================================================================
 * UDP 常量
 * ================================================================ */

#define UDP_BROADCAST_PORT 1566
#define MAX_UDP_IFACES     16

/* ================================================================
 * 内部辅助 — JSON 封装
 * ================================================================ */

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

/* RSSI 到信号等级转换
 * 4 (优秀): RSSI ≥ -80
 * 3 (良好): -90 ≤ RSSI < -80
 * 2 (一般): -100 ≤ RSSI < -90
 * 1 (差): RSSI < -100 */
static uint8_t rssi_to_level(int rssi)
{
    if (rssi >= -80) return 4;
    if (rssi >= -90) return 3;
    if (rssi >= -100) return 2;
    return 1;
}

/* ================================================================
 * UDP 工作线程
 * ================================================================ */

static DWORD WINAPI udp_worker(LPVOID param)
{
    udp_work_t *work = (udp_work_t *)param;
    HWND hwnd = (HWND)work->hwnd;
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
            PostMessage(hwnd, WM_USER + 2, 0,
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
            PostMessage(hwnd, WM_USER + 2, 0, (LPARAM)_strdup(err));
            closesocket(sock);
            free(work);
            return 0;
        }

        char log[1100];
        snprintf(log, sizeof(log), "TX -> %s | %.*s",
                 work->target_ip, work->plen, (const char *)work->payload);
        PostMessage(hwnd, WM_USER + 2, 0, (LPARAM)_strdup(log));

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
            PostMessage(hwnd, WM_USER + 2, 0,
                        (LPARAM)_strdup("Failed to enumerate adapters"));
            free(work);
            return 0;
        }

        if (GetAdaptersAddresses(AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_FRIENDLY_NAME,
                NULL, adapters, &bufLen) != ERROR_SUCCESS) {
            PostMessage(hwnd, WM_USER + 2, 0,
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
            PostMessage(hwnd, WM_USER + 2, 0,
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
                PostMessage(hwnd, WM_USER + 2, 0, (LPARAM)_strdup(err_buf));
                closesocket(sock);
                continue;
            }

            char ip_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &if_ips[i], ip_buf, sizeof(ip_buf));
            char log[1100];
            snprintf(log, sizeof(log), "TX (%s) -> %.*s",
                     ip_buf, work->plen, (const char *)work->payload);
            PostMessage(hwnd, WM_USER + 2, 0, (LPARAM)_strdup(log));

            u_long mode = 1;
            ioctlsocket(sock, FIONBIO, &mode);

            socks[n_socks++] = sock;
        }
    }

    if (n_socks == 0) {
        PostMessage(hwnd, WM_USER + 2, 0,
                    (LPARAM)_strdup("Failed to send on any interface"));
        free(work);
        return 0;
    }

    /* --- select 多路接收，总超时 5 秒，收到响应后立即退出 --- */
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
                            PostMessage(hwnd, WM_USER + 3, 0, (LPARAM)msg);
                        }
                        got_response = 1;
                    }
                }
            }
            if (got_response) break;
        }
    }

    if (!got_response) {
        PostMessage(hwnd, WM_USER + 2, 0,
                    (LPARAM)_strdup("No response (timeout 5s)"));
    }

    for (int i = 0; i < n_socks; i++)
        closesocket(socks[i]);
    free(work);
    return 0;
}

/* ================================================================
 * 内部辅助 — 发送
 * ================================================================ */

/* 广播发送 */
static void udp_send_raw(HWND hwnd, const uint8_t *payload, int plen)
{
    udp_work_t *work = (udp_work_t *)calloc(1, sizeof(udp_work_t));
    if (!work) return;
    work->plen = plen;
    memcpy(work->payload, payload, plen);
    work->hwnd = hwnd;

    CloseHandle(CreateThread(NULL, 0, udp_worker, work, 0, NULL));
}

/* 单播发送到目标 IP */
static void udp_send_unicast(HWND hwnd, const char *ip,
                             const uint8_t *payload, int plen)
{
    udp_work_t *work = (udp_work_t *)calloc(1, sizeof(udp_work_t));
    if (!work) return;
    work->plen = plen;
    memcpy(work->payload, payload, plen);
    snprintf(work->target_ip, sizeof(work->target_ip), "%s", ip);
    work->hwnd = hwnd;

    CloseHandle(CreateThread(NULL, 0, udp_worker, work, 0, NULL));
}

/* 发送 AT 指令 (查询用 GETPARA, 设置用 SETPARA) */
static void udp_send_at_cmd(net_ctx_t *ctx, const char *cmd)
{
    /* 确保 \r\n 结尾 */
    size_t clen = strlen(cmd);
    char full_cmd[256];
    if (clen >= 2 && cmd[clen - 2] == '\r' && cmd[clen - 1] == '\n') {
        snprintf(full_cmd, sizeof(full_cmd), "%s", cmd);
    } else {
        snprintf(full_cmd, sizeof(full_cmd), "%s\r\n", cmd);
    }

    /* 查询 (? 结尾) 用 GETPARA, 设置 (=) 或其他用 SETPARA */
    const char *msg_type = "GETPARA";
    {
        size_t flen = strlen(full_cmd);
        /* 去除尾部 \r\n 后判断 */
        while (flen > 0 && (full_cmd[flen - 1] == '\r' || full_cmd[flen - 1] == '\n'))
            flen--;
        if (flen > 0 && full_cmd[flen - 1] != '?')
            msg_type = "SETPARA";
    }

    const char *mac = strlen(g_dev_mac) > 0 ? g_dev_mac : "D4AD20ED63C4";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VER", "1.0");
    cJSON_AddStringToObject(root, "MSG", msg_type);
    cJSON_AddStringToObject(root, "TYPE", "AT");
    cJSON_AddStringToObject(root, "CMD", full_cmd);
    cJSON_AddStringToObject(root, "USER", "admin");
    cJSON_AddStringToObject(root, "PSW", "admin");
    cJSON_AddStringToObject(root, "MAC", mac);

    uint8_t payload[1024];
    int plen = udp_wrap_json(root, payload, sizeof(payload));
    cJSON_Delete(root);

    if (plen <= 0) { ctx->cb.cfg_log_append(ctx->user_data, "Failed to build payload"); return; }

    /* 有设备地址时单播，否则广播 */
    if (g_dev_addr[0])
        udp_send_unicast(ctx->hwnd, g_dev_addr, payload, plen);
    else
        udp_send_raw(ctx->hwnd, payload, plen);
}

/* ================================================================
 * UDP 响应处理
 * ================================================================ */

/* 从 AT 响应中提取 prefix 之后的值，截止于 \r\n 或空格 */
static int extract_at_value(const char *str, const char *prefix,
                            char *out, int out_size)
{
    const char *p = strstr(str, prefix);
    if (!p) return 0;
    p += (int)strlen(prefix);
    int i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && p[i] != ' ' && i < out_size - 1)
        i++;
    memcpy(out, p, i);
    out[i] = '\0';
    return i;
}

static void udp_process_response(net_ctx_t *ctx,
                                 const char *from_ip, const char *raw)
{
    /* 提取 JSON */
    const char *start = strchr(raw, '{');
    const char *end = strrchr(raw, '}');
    if (!start || !end || end <= start) { ctx->cb.cfg_log_append(ctx->user_data, raw); return; }

    /* 复制 JSON 子串用于解析 */
    int jlen = (int)(end - start + 1);
    char json_buf[2048];
    if (jlen >= (int)sizeof(json_buf)) return;
    memcpy(json_buf, start, jlen);
    json_buf[jlen] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        ctx->cb.cfg_log_append(ctx->user_data, "RX <- (parse error)");
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
            ctx->cb.set_cfg_device_mac(ctx->user_data, txt);
        }
        if (dev && cJSON_IsString(dev)) {
            snprintf(g_dev_name, sizeof(g_dev_name), "%s", dev->valuestring);
            char txt[128]; snprintf(txt, sizeof(txt), "Device: %s", g_dev_name);
            ctx->cb.set_cfg_device_name(ctx->user_data, txt);
        }
        if (sver && cJSON_IsString(sver)) {
            snprintf(g_dev_sw, sizeof(g_dev_sw), "%s", sver->valuestring);
            char txt[128]; snprintf(txt, sizeof(txt), "SW: %s", g_dev_sw);
            ctx->cb.set_cfg_device_sw(ctx->user_data, txt);
        }

        ctx->cb.cfg_log_append(ctx->user_data, "Device found!");
    }

    /* ACK-GETPARA / ACK-SETPARA + CMD=NETDEV: 网络参数响应 */
    if (strcmp(msg, "ACK-GETPARA") == 0 || strcmp(msg, "ACK-SETPARA") == 0) {
        cJSON *cmd_obj = cJSON_GetObjectItemCaseSensitive(root, "CMD");
        if (cmd_obj && cJSON_IsObject(cmd_obj)) {
            cJSON *ip = cJSON_GetObjectItemCaseSensitive(cmd_obj, "IP");
            cJSON *sm = cJSON_GetObjectItemCaseSensitive(cmd_obj, "SM");
            cJSON *gw = cJSON_GetObjectItemCaseSensitive(cmd_obj, "GW");

            if (ip && cJSON_IsString(ip)) {
                snprintf(g_dev_ip, sizeof(g_dev_ip), "%s", ip->valuestring);
                char txt[128]; snprintf(txt, sizeof(txt), "IP: %s", g_dev_ip);
                ctx->cb.set_cfg_ip(ctx->user_data, txt);
            }
            if (sm && cJSON_IsString(sm)) {
                snprintf(g_dev_sm, sizeof(g_dev_sm), "%s", sm->valuestring);
                char txt[128]; snprintf(txt, sizeof(txt), "Mask: %s", g_dev_sm);
                ctx->cb.set_cfg_sm(ctx->user_data, txt);
            }
            if (gw && cJSON_IsString(gw)) {
                snprintf(g_dev_gw, sizeof(g_dev_gw), "%s", gw->valuestring);
                char txt[128]; snprintf(txt, sizeof(txt), "GW: %s", g_dev_gw);
                ctx->cb.set_cfg_gw(ctx->user_data, txt);
            }

            ctx->cb.cfg_log_append(ctx->user_data, "Network parameters received");
        } else {
            /* AT 指令响应 — CMD 是字符串 */
            cJSON *cmd_str = cJSON_GetObjectItemCaseSensitive(root, "CMD");
            if (cmd_str && cJSON_IsString(cmd_str)) {
                const char *val = cmd_str->valuestring;
                char log[600];
                snprintf(log, sizeof(log), "RX <- CMD: %s", val);
                ctx->cb.cfg_log_append(ctx->user_data, log);

                /* 解析 GWID 响应: 匹配 "GWID:" 或 "+GWID:" */
                const char *p = strstr(val, "GWID:");
                if (p) {
                    p += 5; /* skip "GWID:" */
                    int i = 0;
                    while (p[i] && p[i] != '\r' && p[i] != '\n' && p[i] != ' ' && i < 15) i++;
                    snprintf(g_dev_gwid, sizeof(g_dev_gwid), "%.*s", i, p);
                    char txt[128];
                    snprintf(txt, sizeof(txt), "GWID: %s", g_dev_gwid);
                    ctx->cb.set_cfg_gwid(ctx->user_data, txt);
                }

                /* 解析 CSQ 响应: "+CSQ:<net>,<signal>" */
                const char *csq = strstr(val, "+CSQ:");
                if (csq) {
                    csq += 5;
                    int net_val = 0, sig_val = 0;
                    if (sscanf(csq, "%d,%d", &net_val, &sig_val) == 2) {
                        const char *net_str = (net_val == 2) ? "2G" :
                                              (net_val == 3) ? "3G" :
                                              (net_val == 4) ? "4G" : "?";
                        char txt[128];
                        snprintf(txt, sizeof(txt), "Signal: %s (%d)", net_str, sig_val);
                        ctx->cb.set_cfg_csq(ctx->user_data, txt);
                    }
                }

                /* 解析 DHCP 响应: "+DHCP:<status>" */
                {
                    char v[32];
                    if (extract_at_value(val, "+DHCP:", v, sizeof(v))) {
                        char txt[64];
                        snprintf(txt, sizeof(txt), "DHCP: %s", v);
                        ctx->cb.set_cfg_dhcp(ctx->user_data, txt);
                    }
                }

                /* 解析 GWIP 响应: "+GWIP:<address>" */
                {
                    char v[64];
                    if (extract_at_value(val, "+GWIP:", v, sizeof(v))) {
                        if (strcmp(v, "OK") != 0) {
                            snprintf(g_dev_ip, sizeof(g_dev_ip), "%s", v);
                            char txt[128];
                            snprintf(txt, sizeof(txt), "IP: %s", v);
                            ctx->cb.set_cfg_ip(ctx->user_data, txt);
                        }
                    }
                }

                /* 解析 GW 响应: "+GW:<address>" */
                {
                    char v[64];
                    if (extract_at_value(val, "+GW:", v, sizeof(v))) {
                        if (strcmp(v, "OK") != 0) {
                            snprintf(g_dev_gw, sizeof(g_dev_gw), "%s", v);
                            char txt[128];
                            snprintf(txt, sizeof(txt), "GW: %s", v);
                            ctx->cb.set_cfg_gw(ctx->user_data, txt);
                        }
                    }
                }

                /* 解析 MASK 响应: "+MASK:<address>" */
                {
                    char v[64];
                    if (extract_at_value(val, "+MASK:", v, sizeof(v))) {
                        if (strcmp(v, "OK") != 0) {
                            snprintf(g_dev_sm, sizeof(g_dev_sm), "%s", v);
                            char txt[128];
                            snprintf(txt, sizeof(txt), "Mask: %s", v);
                            ctx->cb.set_cfg_sm(ctx->user_data, txt);
                        }
                    }
                }

                /* 解析 OPTION 响应: "+OPTION:<mode>" */
                {
                    char v[16];
                    if (extract_at_value(val, "+OPTION:", v, sizeof(v))) {
                        if (strcmp(v, "OK") != 0) {
                            int mode = atoi(v);
                            const char *names[] = {"socket", "serial", "mqtt", "ali_cloud", "usr_cloud"};
                            const char *name = (mode >= 0 && mode <= 4) ? names[mode] : "?";
                            char txt[64];
                            snprintf(txt, sizeof(txt), "Mode: %s (%d)", name, mode);
                            ctx->cb.set_cfg_option(ctx->user_data, txt);
                            ctx->cb.set_cfg_option_edit(ctx->user_data, v);
                        }
                    }
                }

                /* 解析 NWMODE 响应: "+NWMODE:<status>" */
                {
                    char v[16];
                    if (extract_at_value(val, "+NWMODE:", v, sizeof(v))) {
                        if (strcmp(v, "OK") != 0)
                            ctx->cb.set_cfg_nwmode(ctx->user_data, v);
                    }
                }

                /* 解析 TTMODE 响应: "+TTMODE:<mode>" */
                {
                    char v[16];
                    if (extract_at_value(val, "+TTMODE:", v, sizeof(v))) {
                        if (strcmp(v, "OK") != 0)
                            ctx->cb.set_cfg_ttmode(ctx->user_data, v);
                    }
                }

                /* 解析 WMODE 响应: "+WMODE:<mode>" */
                {
                    char v[16];
                    if (extract_at_value(val, "+WMODE:", v, sizeof(v))) {
                        if (strcmp(v, "OK") != 0)
                            ctx->cb.set_cfg_wmode(ctx->user_data, v);
                    }
                }

                /* 解析 UPWID 响应: "+UPWID:<status>" */
                {
                    char v[16];
                    if (extract_at_value(val, "+UPWID:", v, sizeof(v))) {
                        if (strcmp(v, "OK") != 0)
                            ctx->cb.set_cfg_upwid(ctx->user_data, v);
                    }
                }

                /* 解析 CH 响应: "+CH<n>:<channel>" */
                {
                    const char *p = strstr(val, "+CH");
                    if (p && p[3] >= '0' && p[3] <= '9') {
                        const char *colon = strchr(p + 3, ':');
                        if (colon) {
                            const char *v = colon + 1;
                            if (strncmp(v, "OK", 2) != 0)
                                ctx->cb.set_cfg_ch(ctx->user_data, v);
                        }
                    }
                }

                /* 解析 SPD 响应: "+SPD<n>:<speed>" */
                {
                    const char *p = strstr(val, "+SPD");
                    if (p && p[4] >= '0' && p[4] <= '9') {
                        const char *colon = strchr(p + 4, ':');
                        if (colon) {
                            const char *v = colon + 1;
                            if (strncmp(v, "OK", 2) != 0)
                                ctx->cb.set_cfg_spd(ctx->user_data, v);
                        }
                    }
                }

                /* 解析 PWR 响应: "+PWR<n>:<power>" */
                {
                    const char *p = strstr(val, "+PWR");
                    if (p && p[4] >= '0' && p[4] <= '9') {
                        const char *colon = strchr(p + 4, ':');
                        if (colon) {
                            const char *v = colon + 1;
                            if (strncmp(v, "OK", 2) != 0)
                                ctx->cb.set_cfg_pwr(ctx->user_data, v);
                        }
                    }
                }

                /* 解析 NINFO 响应: "+NINFO:<info>"
                 * 格式: 网络id,节点id,?,SNR,RSSI,...
                 * 示例: "NINFO:001,001DADC0,1,+012,+0,000000F8,00000000,1,2026/04/20-18:28:42,0000000000,000"
                 * 其中 +012 是 SNR, +0 是 RSSI (实际值可能是 -80 的特殊表示)
                 * 若有 pending RSSI 请求，提取信号值回复 TCP */
                {
                    const char *p = strstr(val, "+NINFO:");
                    if (p) {
                        const char *info = p + 7;
                        ctx->cb.cfg_log_append(ctx->user_data, info);

                        if (g_pending_rssi_nid != 0) {
                            /* 按逗号分隔解析，第4字段 SNR，第5字段 RSSI */
                            char *buf = _strdup(info);
                            char *token = strtok(buf, ",");
                            int field_idx = 0;
                            int rssi_val = -120;

                            while (token) {
                                field_idx++;
                                if (field_idx == 4) {
                                    /* SNR 字段，跳过 */
                                } else if (field_idx == 5) {
                                    rssi_val = atoi(token);
                                    break;
                                }
                                token = strtok(NULL, ",");
                            }
                            free(buf);

                            uint8_t rssi_level = rssi_to_level(rssi_val);
                            net_send_rssi_response(ctx, g_pending_rssi_nid, rssi_level);
                            g_pending_rssi_nid = 0;
                        }
                    }
                }
            }
        }
    }

    /* 格式化打印完整 JSON */
    char *fmt = cJSON_Print(root);
    if (fmt) {
        ctx->cb.cfg_log_append(ctx->user_data, fmt);
        cJSON_free(fmt);
    }

    cJSON_Delete(root);
}

/* ================================================================
 * 公共 API
 * ================================================================ */

/* 搜索设备 */
void net_cfg_search(net_ctx_t *ctx)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VER", "1.0");
    cJSON_AddStringToObject(root, "MSG", "SEARCH");
    cJSON_AddStringToObject(root, "TYPE", "LORA");

    uint8_t payload[1024];
    int plen = udp_wrap_json(root, payload, sizeof(payload));
    cJSON_Delete(root);

    if (plen <= 0) { ctx->cb.cfg_log_append(ctx->user_data, "Failed to build payload"); return; }

    udp_send_raw(ctx->hwnd, payload, plen);
}

/* 获取网络参数 */
void net_cfg_get_net(net_ctx_t *ctx)
{
    if (strlen(g_dev_mac) == 0) {
        ctx->cb.show_error(ctx->user_data, "Error", "Please search devices first");
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

    if (plen <= 0) { ctx->cb.cfg_log_append(ctx->user_data, "Failed to build payload"); return; }

    /* 有设备地址时单播，否则广播 */
    if (g_dev_addr[0])
        udp_send_unicast(ctx->hwnd, g_dev_addr, payload, plen);
    else
        udp_send_raw(ctx->hwnd, payload, plen);
}

/* 发送自定义 AT 命令 */
void net_cfg_send(net_ctx_t *ctx, const char *cmd)
{
    if (strlen(cmd) == 0) {
        ctx->cb.show_error(ctx->user_data, "Error", "Please enter AT command");
        return;
    }
    udp_send_at_cmd(ctx, cmd);
}

/* 快捷 AT 命令 */
void net_cfg_quick(net_ctx_t *ctx, const char *cmd)
{
    ctx->cb.set_cfg_cmd_edit(ctx->user_data, cmd);
    udp_send_at_cmd(ctx, cmd);
}

/* WM_UDP_LOG 处理 */
void net_on_udp_log(net_ctx_t *ctx, const char *text)
{
    ctx->cb.cfg_log_append(ctx->user_data, text);
}

/* WM_UDP_RX 处理 */
void net_on_udp_rx(net_ctx_t *ctx, udp_rx_msg_t *msg)
{
    udp_process_response(ctx, msg->from_ip, msg->data);
    free(msg);
}
