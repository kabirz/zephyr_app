/*
 * example.c — LoRa Gateway SDK 使用示例
 *
 * 演示: 初始化 → 搜索设备 → 连接 TCP → 收发数据 → 断开 → 清理
 *
 * 编译 (MSVC):
 *   cl example.c lora_gateway_sdk.lib
 *
 * 编译 (MinGW):
 *   x86_64-w64-mingw32-gcc example.c -L../build -llora_gateway_sdk -o example.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "lora_sdk.h"

static volatile int g_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ================================================================
 * 回调函数
 * ================================================================ */

static void on_conn_state(void *ud, enum lora_sdk_conn_state state)
{
    (void)ud;
    const char *names[] = { "DISCONNECTED", "CONNECTING", "CONNECTED" };
    printf("[CONN] %s\n", names[state]);
}

static void on_frame(void *ud, uint32_t nid,
                     const uint8_t *payload, uint16_t len)
{
    (void)ud;
    printf("[FRAME] NID=%08X  len=%u  type=0x%02X",
           nid, len, len > 0 ? payload[0] : 0);

    if (len > 0 && payload[0] == 0x01 && len >= 9) {
        /* HANDLER 遥测: [0x01][X 2B BE][Y 2B BE][btn][0xFF 3B] */
        int16_t x = (int16_t)((payload[1] << 8) | payload[2]);
        int16_t y = (int16_t)((payload[3] << 8) | payload[4]);
        uint8_t btn = payload[5] & 0x01;
        printf("  -> Telemetry X=%d Y=%d Btn=%s",
               x, y, btn ? "Released" : "Pressed");
    }
    printf("\n");
}

static void on_device_found(void *ud, const char *mac,
                            const char *name, const char *sw,
                            const char *ip)
{
    (void)ud;
    printf("[DEVICE] MAC=%s  Name=%s  SW=%s  IP=%s\n",
           mac, name, sw, ip);
}

static void on_at_response(void *ud, const char *resp)
{
    (void)ud;
    printf("[AT] %s\n", resp);
}

static void on_net_params(void *ud, const char *ip,
                          const char *mask, const char *gw)
{
    (void)ud;
    printf("[NET] IP=%s  Mask=%s  GW=%s\n", ip, mask, gw);
}

static void on_log(void *ud, const char *msg)
{
    (void)ud;
    printf("[LOG] %s\n", msg);
}

static void on_hex_dump(void *ud, const char *prefix,
                        const uint8_t *data, int len)
{
    (void)ud;
    printf("[HEX] %s (%d bytes):", prefix, len);
    for (int i = 0; i < len && i < 32; i++)
        printf(" %02X", data[i]);
    if (len > 32) printf(" ...");
    printf("\n");
}

static void on_error(void *ud, const char *msg)
{
    (void)ud;
    printf("[ERR] %s\n", msg);
}

/* ================================================================
 * 控制台命令
 * ================================================================ */

static void print_usage(void)
{
    printf("\n"
           "Commands:\n"
           "  s              - Search devices (UDP broadcast)\n"
           "  c <ip> [port]  - Connect to gateway (default port 8899)\n"
           "  d              - Disconnect\n"
           "  n              - Get network parameters\n"
           "  a <at_cmd>     - Send AT command (e.g. a AT+NINFO?)\n"
           "  r              - Send test RSSI response\n"
           "  q              - Quit\n"
           "\n");
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sigint_handler);

    lora_sdk_callbacks_t cbs = {0};
    cbs.on_conn_state   = on_conn_state;
    cbs.on_frame        = on_frame;
    cbs.on_device_found = on_device_found;
    cbs.on_at_response  = on_at_response;
    cbs.on_net_params   = on_net_params;
    cbs.on_log          = on_log;
    cbs.on_hex_dump     = on_hex_dump;
    cbs.on_error        = on_error;

    lora_sdk_t *sdk = lora_sdk_init(&cbs, NULL);
    if (!sdk) {
        fprintf(stderr, "Failed to init SDK\n");
        return 1;
    }

    printf("LoRa Gateway SDK Example\n");
    print_usage();

    char line[256];
    while (g_running) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        /* 去除尾部换行 */
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '\0')
            continue;

        switch (line[0]) {
        case 's':
            printf("Searching devices...\n");
            lora_sdk_search_devices(sdk);
            break;

        case 'c': {
            char ip[64] = {0};
            int port = 8899;
            int n = sscanf(line + 1, " %63s %d", ip, &port);
            if (n < 1 || ip[0] == '\0') {
                printf("Usage: c <ip> [port]\n");
                break;
            }
            printf("Connecting to %s:%d...\n", ip, port);
            lora_sdk_connect(sdk, ip, port);
            break;
        }

        case 'd':
            lora_sdk_disconnect(sdk);
            break;

        case 'n':
            lora_sdk_get_net_params(sdk);
            break;

        case 'a': {
            const char *cmd = line + 1;
            while (*cmd == ' ') cmd++;
            if (*cmd == '\0') {
                printf("Usage: a <at_cmd>\n");
                break;
            }
            lora_sdk_send_at(sdk, cmd);
            break;
        }

        case 'r': {
            /* 发送测试 RSSI 响应 */
            lora_sdk_send_rssi_response(sdk, 0, 10, -50, 0);
            printf("Sent RSSI response\n");
            break;
        }

        case 'q':
            g_running = 0;
            break;

        default:
            print_usage();
            break;
        }
    }

    printf("Cleaning up...\n");
    lora_sdk_cleanup(sdk);
    printf("Done.\n");
    return 0;
}
