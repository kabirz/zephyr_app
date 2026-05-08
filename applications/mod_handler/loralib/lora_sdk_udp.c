/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk_udp.c — UDP device discovery and LoRa module configuration
 *
 * USR-LG210-L gateway: broadcast discovery, network params, AT commands.
 * Worker threads invoke callbacks directly — no HWND/PostMessage.
 */

#include "lora_sdk_internal.h"
#include "cJSON.h"

#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "iphlpapi.lib")

#define UDP_PORT     1566
#define UDP_TIMEOUT  5000

/* ================================================================
 * UDP worker thread parameter
 * ================================================================ */

typedef struct {
    int       plen;
    uint8_t   payload[2048];
    lora_sdk_t *sdk;
} udp_work_t;

/* ================================================================
 * JSON helpers
 * ================================================================ */

static int wrap_json(cJSON *root, uint8_t *out, int out_size)
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

/* 从 AT 响应中提取 prefix 之后的值 */
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

/* ================================================================
 * UDP response processing — JSON parse + callback dispatch
 * ================================================================ */

static void process_response(lora_sdk_t *sdk,
                             const char *from_ip, const char *raw)
{
    const char *start = strchr(raw, '{');
    const char *end = strrchr(raw, '}');
    if (!start || !end || end <= start) {
        SDK_CALL(sdk, on_log, raw);
        return;
    }

    int jlen = (int)(end - start + 1);
    char json_buf[2048];
    if (jlen >= (int)sizeof(json_buf)) return;
    memcpy(json_buf, start, jlen);
    json_buf[jlen] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        SDK_CALL(sdk, on_log, "RX <- (JSON parse error)");
        return;
    }

    const char *msg = "";
    cJSON *msg_item = cJSON_GetObjectItemCaseSensitive(root, "MSG");
    if (msg_item && cJSON_IsString(msg_item)) msg = msg_item->valuestring;

    /* --- ACK-SEARCH: 设备发现 --- */
    if (strcmp(msg, "ACK-SEARCH") == 0) {
        snprintf(sdk->dev_addr, sizeof(sdk->dev_addr), "%s", from_ip);

        cJSON *mac = cJSON_GetObjectItemCaseSensitive(root, "MAC");
        cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "DEV");
        cJSON *sver = cJSON_GetObjectItemCaseSensitive(root, "SVER");

        if (mac && cJSON_IsString(mac))
            snprintf(sdk->dev_mac, sizeof(sdk->dev_mac), "%s", mac->valuestring);
        if (dev && cJSON_IsString(dev))
            snprintf(sdk->dev_name, sizeof(sdk->dev_name), "%s", dev->valuestring);
        if (sver && cJSON_IsString(sver))
            snprintf(sdk->dev_sw, sizeof(sdk->dev_sw), "%s", sver->valuestring);

        /* 所有设备信息已写入，触发回调 */
        SDK_CALL(sdk, on_device_found,
                 sdk->dev_mac, sdk->dev_name, sdk->dev_sw, from_ip);
        SDK_CALL(sdk, on_log, "Device found!");
    }

    /* --- ACK-GETPARA / ACK-SETPARA --- */
    if (strcmp(msg, "ACK-GETPARA") == 0 || strcmp(msg, "ACK-SETPARA") == 0) {
        cJSON *cmd_obj = cJSON_GetObjectItemCaseSensitive(root, "CMD");
        if (cmd_obj && cJSON_IsObject(cmd_obj)) {
            /* 网络参数响应 (CMD is JSON object with IP/SM/GW) */
            cJSON *ip = cJSON_GetObjectItemCaseSensitive(cmd_obj, "IP");
            cJSON *sm = cJSON_GetObjectItemCaseSensitive(cmd_obj, "SM");
            cJSON *gw = cJSON_GetObjectItemCaseSensitive(cmd_obj, "GW");

            if (ip && cJSON_IsString(ip))
                snprintf(sdk->dev_ip, sizeof(sdk->dev_ip), "%s", ip->valuestring);
            if (sm && cJSON_IsString(sm))
                snprintf(sdk->dev_sm, sizeof(sdk->dev_sm), "%s", sm->valuestring);
            if (gw && cJSON_IsString(gw))
                snprintf(sdk->dev_gw, sizeof(sdk->dev_gw), "%s", gw->valuestring);

            SDK_CALL(sdk, on_net_params, sdk->dev_ip, sdk->dev_sm, sdk->dev_gw);
            SDK_CALL(sdk, on_log, "Network parameters received");
        } else if (cmd_obj && cJSON_IsString(cmd_obj)) {
            /* AT 指令响应 (CMD is string) */
            const char *val = cmd_obj->valuestring;
            char log[600];
            snprintf(log, sizeof(log), "RX <- CMD: %s", val);
            SDK_CALL(sdk, on_at_response, val);
            SDK_CALL(sdk, on_log, log);

            /* 解析 GWID */
            {
                const char *p = strstr(val, "GWID:");
                if (p) {
                    p += 5;
                    int i = 0;
                    while (p[i] && p[i] != '\r' && p[i] != '\n' && p[i] != ' ' && i < 15) i++;
                    snprintf(sdk->dev_gwid, sizeof(sdk->dev_gwid), "%.*s", i, p);
                }
            }

            /* 解析 NINFO — 用于 RSSI 响应 */
            {
                const char *p = strstr(val, "+NINFO:");
                if (p) {
                    const char *info = p + 7;
                    int info_len = (int)strlen(info);
                    while (info_len > 0 && (info[info_len - 1] == '\r' ||
                           info[info_len - 1] == '\n' ||
                           info[info_len - 1] == ' '))
                        info_len--;

                    if (info_len > 0)
                        SDK_CALL(sdk, on_log, info);

                    if (sdk->pending_rssi_nid != 0) {
                        int snr_val = 0, rssi_val = -120;

                        if (info_len > 0) {
                            int field_idx = 0;
                            const char *cur = info;
                            while (*cur && *cur != '\r' && *cur != '\n' && field_idx < 5) {
                                field_idx++;
                                const char *endp = cur;
                                while (*endp && *endp != ',' && *endp != '\r' && *endp != '\n')
                                    endp++;
                                int flen = (int)(endp - cur);
                                if (field_idx == 4 && flen > 0) snr_val = atoi(cur);
                                if (field_idx == 5 && flen > 0) rssi_val = atoi(cur);
                                if (*endp != ',') break;
                                cur = endp + 1;
                            }
                        }

                        uint8_t snr_raw = (uint8_t)(int8_t)snr_val;
                        uint8_t rssi_raw = (uint8_t)(int8_t)rssi_val;
                        sdk_tcp_send_rssi(sdk, sdk->pending_rssi_nid,
                                          snr_raw, rssi_raw, (uint8_t)sdk->test_flag);
                        sdk->pending_rssi_nid = 0;
                    }
                }
            }
        }
    }

    /* 格式化打印完整 JSON */
    char *fmt = cJSON_Print(root);
    if (fmt) {
        SDK_CALL(sdk, on_log, fmt);
        cJSON_free(fmt);
    }

    cJSON_Delete(root);
}

/* ================================================================
 * UDP worker thread — 广播发送 + 5s 等待响应
 * ================================================================ */

static DWORD WINAPI udp_worker(LPVOID param)
{
    udp_work_t *work = (udp_work_t *)param;
    lora_sdk_t *sdk = work->sdk;
    int got_response = 0;

    SOCKET socks[SDK_UDP_MAX_IFACES];
    int n_socks = 0;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(UDP_PORT);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    if (sdk->local_if_ip[0]) {
        /* 已知网卡 */
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            SDK_CALL(sdk, on_log, "Failed to create UDP socket");
            free(work);
            return 0;
        }

        BOOL bcast = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast));

        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        inet_pton(AF_INET, sdk->local_if_ip, &local.sin_addr);

        if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            char err[128];
            snprintf(err, sizeof(err), "bind %s failed: %d",
                     sdk->local_if_ip, WSAGetLastError());
            SDK_CALL(sdk, on_log, err);
            closesocket(sock);
            free(work);
            return 0;
        }

        if (sendto(sock, (const char *)work->payload, work->plen, 0,
                   (struct sockaddr *)&dest, sizeof(dest)) == SOCKET_ERROR) {
            char err[128];
            snprintf(err, sizeof(err), "sendto failed on %s: %d",
                     sdk->local_if_ip, WSAGetLastError());
            SDK_CALL(sdk, on_log, err);
            closesocket(sock);
            free(work);
            return 0;
        }

        char log[1100];
        snprintf(log, sizeof(log), "TX (%s) -> %.*s",
                 sdk->local_if_ip, work->plen, (const char *)work->payload);
        SDK_CALL(sdk, on_log, log);

        u_long nbio = 1;
        ioctlsocket(sock, FIONBIO, &nbio);
        socks[n_socks++] = sock;
    } else {
        /* 枚举所有活跃 IPv4 网卡 */
        ULONG bufLen = 0;
        GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, NULL, &bufLen);

        PIP_ADAPTER_ADDRESSES adapters = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
        if (!adapters) {
            SDK_CALL(sdk, on_log, "Failed to enumerate adapters");
            free(work);
            return 0;
        }

        if (GetAdaptersAddresses(AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_FRIENDLY_NAME,
                NULL, adapters, &bufLen) != ERROR_SUCCESS) {
            SDK_CALL(sdk, on_log, "GetAdaptersAddresses failed");
            free(adapters);
            free(work);
            return 0;
        }

        struct in_addr if_ips[SDK_UDP_MAX_IFACES];
        int n_if = 0;

        for (PIP_ADAPTER_ADDRESSES aa = adapters; aa && n_if < SDK_UDP_MAX_IFACES; aa = aa->Next) {
            if (aa->OperStatus != IfOperStatusUp) continue;
            if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS ua = aa->FirstUnicastAddress;
                 ua && n_if < SDK_UDP_MAX_IFACES; ua = ua->Next) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ua->Address.lpSockaddr;
                if (sa->sin_family != AF_INET) continue;
                if_ips[n_if++] = sa->sin_addr;
            }
        }
        free(adapters);

        if (n_if == 0) {
            SDK_CALL(sdk, on_log, "No active IPv4 interfaces found");
            free(work);
            return 0;
        }

        for (int i = 0; i < n_if; i++) {
            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) continue;

            BOOL bcast = TRUE;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast));

            struct sockaddr_in local;
            memset(&local, 0, sizeof(local));
            local.sin_family = AF_INET;
            local.sin_addr = if_ips[i];
            if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
                closesocket(sock);
                continue;
            }

            if (sendto(sock, (const char *)work->payload, work->plen, 0,
                       (struct sockaddr *)&dest, sizeof(dest)) == SOCKET_ERROR) {
                char ip_buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &if_ips[i], ip_buf, sizeof(ip_buf));
                char err[128];
                snprintf(err, sizeof(err), "sendto failed on %s: %d",
                         ip_buf, WSAGetLastError());
                SDK_CALL(sdk, on_log, err);
                closesocket(sock);
                continue;
            }

            char ip_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &if_ips[i], ip_buf, sizeof(ip_buf));
            char log[1100];
            snprintf(log, sizeof(log), "TX (%s) -> %.*s",
                     ip_buf, work->plen, (const char *)work->payload);
            SDK_CALL(sdk, on_log, log);

            u_long nbio = 1;
            ioctlsocket(sock, FIONBIO, &nbio);
            socks[n_socks++] = sock;
        }
    }

    if (n_socks == 0) {
        SDK_CALL(sdk, on_log, "Failed to send on any interface");
        free(work);
        return 0;
    }

    /* select 多路接收 */
    DWORD start = GetTickCount();
    char buf[2048];

    while (1) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= UDP_TIMEOUT) break;

        struct timeval tv = { 0, 200000 };
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

                        /* 记录本端网卡 IP */
                        struct sockaddr_in local_addr;
                        int local_len = sizeof(local_addr);
                        if (sdk->local_if_ip[0] == '\0' &&
                            getsockname(socks[i], (struct sockaddr *)&local_addr,
                                        &local_len) == 0) {
                            inet_ntop(AF_INET, &local_addr.sin_addr,
                                      sdk->local_if_ip, sizeof(sdk->local_if_ip));
                        }

                        char from_ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));
                        process_response(sdk, from_ip, buf);
                        got_response = 1;
                    }
                }
            }
            if (got_response) break;
        }
    }

    if (!got_response)
        SDK_CALL(sdk, on_log, "No response (timeout 5s)");

    for (int i = 0; i < n_socks; i++)
        closesocket(socks[i]);
    free(work);
    return 0;
}

/* ================================================================
 * Internal helpers — send via worker thread
 * ================================================================ */

static void udp_send_raw(lora_sdk_t *sdk, const uint8_t *payload, int plen)
{
    udp_work_t *work = (udp_work_t *)calloc(1, sizeof(udp_work_t));
    if (!work) return;
    work->plen = plen;
    memcpy(work->payload, payload, plen);
    work->sdk = sdk;
    CloseHandle(CreateThread(NULL, 0, udp_worker, work, 0, NULL));
}

static void udp_send_at_cmd(lora_sdk_t *sdk, const char *cmd)
{
    size_t clen = strlen(cmd);
    char full_cmd[256];
    if (clen >= 2 && cmd[clen - 2] == '\r' && cmd[clen - 1] == '\n')
        snprintf(full_cmd, sizeof(full_cmd), "%s", cmd);
    else
        snprintf(full_cmd, sizeof(full_cmd), "%s\r\n", cmd);

    const char *msg_type = "GETPARA";
    {
        size_t flen = strlen(full_cmd);
        while (flen > 0 && (full_cmd[flen - 1] == '\r' || full_cmd[flen - 1] == '\n'))
            flen--;
        if (flen > 0 && full_cmd[flen - 1] != '?')
            msg_type = "SETPARA";
    }

    const char *mac = sdk->dev_mac[0] ? sdk->dev_mac : "D4AD20ED63C4";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VER", "1.0");
    cJSON_AddStringToObject(root, "MSG", msg_type);
    cJSON_AddStringToObject(root, "TYPE", "AT");
    cJSON_AddStringToObject(root, "CMD", full_cmd);
    cJSON_AddStringToObject(root, "USER", "admin");
    cJSON_AddStringToObject(root, "PSW", "admin");
    cJSON_AddStringToObject(root, "MAC", mac);

    uint8_t payload[1024];
    int plen = wrap_json(root, payload, sizeof(payload));
    cJSON_Delete(root);

    if (plen <= 0) {
        SDK_CALL(sdk, on_log, "Failed to build UDP payload");
        return;
    }

    udp_send_raw(sdk, payload, plen);
}

/* ================================================================
 * Public UDP functions
 * ================================================================ */

void sdk_udp_search(lora_sdk_t *sdk)
{
    sdk->local_if_ip[0] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VER", "1.0");
    cJSON_AddStringToObject(root, "MSG", "SEARCH");
    cJSON_AddStringToObject(root, "TYPE", "LORA");

    uint8_t payload[1024];
    int plen = wrap_json(root, payload, sizeof(payload));
    cJSON_Delete(root);

    if (plen <= 0) {
        SDK_CALL(sdk, on_log, "Failed to build search payload");
        return;
    }

    udp_send_raw(sdk, payload, plen);
}

void sdk_udp_get_net(lora_sdk_t *sdk)
{
    if (sdk->dev_mac[0] == '\0') {
        SDK_CALL(sdk, on_error, "Please search devices first");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VER", "1.0");
    cJSON_AddStringToObject(root, "MSG", "GETPARA");
    cJSON_AddStringToObject(root, "TYPE", "JSON");
    cJSON_AddStringToObject(root, "CMD", "NETDEV");
    cJSON_AddStringToObject(root, "USER", "admin");
    cJSON_AddStringToObject(root, "PSW", "admin");
    cJSON_AddStringToObject(root, "MAC", sdk->dev_mac);

    uint8_t payload[1024];
    int plen = wrap_json(root, payload, sizeof(payload));
    cJSON_Delete(root);

    if (plen <= 0) {
        SDK_CALL(sdk, on_log, "Failed to build payload");
        return;
    }

    udp_send_raw(sdk, payload, plen);
}

void sdk_udp_send_at(lora_sdk_t *sdk, const char *cmd)
{
    if (!cmd || !cmd[0]) {
        SDK_CALL(sdk, on_error, "Empty AT command");
        return;
    }
    udp_send_at_cmd(sdk, cmd);
}
