# LoRa Gateway SDK

Windows DLL，用于通过 TCP 接收 LoRa 网关转发的数据帧，通过 UDP 进行网关设备发现和参数配置。

适用于 USR-LG210-L 网关，配合嵌入式端 `mod_handler` 使用。

## 功能

| 功能 | 说明 |
|------|------|
| TCP 数据通信 | 非阻塞连接、帧接收/发送、CRC16 校验 |
| UDP 设备发现 | 广播搜索局域网内网关、自动锁定网卡 |
| UDP 参数配置 | 查询/设置网络参数、透传 AT 指令 |
| 回调通知 | 连接状态、数据帧、设备发现、日志等事件回调 |

## 目录结构

```
loralib/
  lora_sdk.h              公共 API 头文件（客户集成只需此文件 + DLL）
  lora_sdk.c              DLL 入口 + 生命周期管理
  lora_sdk_internal.h     内部类型（不对外分发）
  lora_sdk_net.c          帧协议辅助函数
  lora_sdk_tcp.c          TCP 连接/收发/帧解析
  lora_sdk_udp.c          UDP 设备发现/配置/AT 指令
  crc16.c / crc16.h       CRC16-CCITT
  cJSON.c / cJSON.h       JSON 解析
  CMakeLists.txt          构建配置
  toolchain-mingw64.cmake Linux 交叉编译工具链
  examples/
    example.c             交互式示例程序
    CMakeLists.txt        示例构建配置
```

## 构建

### Linux 交叉编译（推荐）

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake
cmake --build . -j$(nproc)
```

产物：
- `build/liblora_gateway_sdk.dll` — SDK 动态库
- `build/examples/example.exe` — 示例程序

### Windows MSVC

```bat
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A Win32
cmake --build . --config Release
```

## 快速集成

### 1. 包含头文件，链接 DLL

```c
#include "lora_sdk.h"
```

编译时链接 `lora_gateway_sdk.lib`（MSVC）或 `-llora_gateway_sdk`（MinGW）。
运行时需要 `lora_gateway_sdk.dll` 在可执行文件同目录或 PATH 中。

### 2. 实现回调函数

```c
void on_conn_state(void *ud, enum lora_sdk_conn_state state) {
    printf("连接状态: %d\n", state);
}

void on_frame(void *ud, uint32_t nid,
              const uint8_t *payload, uint16_t len) {
    uint8_t type = len > 0 ? payload[0] : 0;
    printf("收到帧 NID=%08X type=0x%02X len=%u\n", nid, type, len);
}

void on_device_found(void *ud, const char *mac,
                     const char *name, const char *sw,
                     const char *ip) {
    printf("发现设备: %s (%s) @ %s\n", name, mac, ip);
}

void on_log(void *ud, const char *msg) {
    printf("[LOG] %s\n", msg);
}

void on_error(void *ud, const char *msg) {
    fprintf(stderr, "[ERR] %s\n", msg);
}
```

### 3. 初始化并使用

```c
lora_sdk_callbacks_t cbs = {0};
cbs.on_conn_state   = on_conn_state;
cbs.on_frame        = on_frame;
cbs.on_device_found = on_device_found;
cbs.on_log          = on_log;
cbs.on_error        = on_error;

lora_sdk_t *sdk = lora_sdk_init(&cbs, NULL);

/* 搜索设备 */
lora_sdk_search_devices(sdk);

/* 连接网关 */
lora_sdk_connect(sdk, "192.168.1.100", 8899);

/* ... 数据收发 ... */

/* 清理 */
lora_sdk_disconnect(sdk);
lora_sdk_cleanup(sdk);
```

## API 参考

### 生命周期

| 函数 | 说明 |
|------|------|
| `lora_sdk_init(callbacks, user_data)` | 初始化 SDK，返回实例句柄 |
| `lora_sdk_cleanup(sdk)` | 断开连接并释放资源 |

### TCP 操作

| 函数 | 说明 |
|------|------|
| `lora_sdk_connect(sdk, ip, port)` | 异步连接网关 |
| `lora_sdk_disconnect(sdk)` | 断开连接 |
| `lora_sdk_conn_state(sdk)` | 查询连接状态 |
| `lora_sdk_send_frame(sdk, nid, data, len)` | 发送数据帧 |
| `lora_sdk_send_rssi_response(sdk, nid, snr, rssi, flag)` | 发送 RSSI 响应 |

### UDP 操作

| 函数 | 说明 |
|------|------|
| `lora_sdk_search_devices(sdk)` | 广播搜索设备（5s 超时） |
| `lora_sdk_get_net_params(sdk)` | 查询设备网络参数 |
| `lora_sdk_send_at(sdk, cmd)` | 透传 AT 指令 |

### 工具函数

| 函数 | 说明 |
|------|------|
| `lora_sdk_build_frame(out, size, nid, data, len)` | 构造原始协议帧 |

## 回调说明

所有回调从后台工作线程调用。如果需要在 UI 线程处理（如更新控件），需自行 marshal。

```c
typedef struct {
    void (*on_conn_state)(void *ud, enum lora_sdk_conn_state state);
    void (*on_frame)(void *ud, uint32_t nid, const uint8_t *payload, uint16_t len);
    void (*on_device_found)(void *ud, const char *mac, const char *name,
                            const char *sw, const char *ip);
    void (*on_at_response)(void *ud, const char *at_response);
    void (*on_net_params)(void *ud, const char *ip, const char *mask, const char *gw);
    void (*on_log)(void *ud, const char *message);
    void (*on_hex_dump)(void *ud, const char *prefix, const uint8_t *data, int len);
    void (*on_error)(void *ud, const char *message);
} lora_sdk_callbacks_t;
```

所有回调指针均可为 NULL（不注册则不触发）。

## 帧格式

TCP 数据帧封装：

```
发送: [NID 4B][0xAA][0x55][统一帧][\r\n]
接收: [0xAA][0x55][统一帧][\r\n]
```

统一帧：

```
[NID 4B BE][Length 2B BE][Data NB][CRC16 2B BE]
```

Data 首字节为类型标识：

| 值 | 类型 | 说明 |
|----|------|------|
| 0x01 | HANDLER | 遥测数据（X/Y 角度 + 按键） |
| 0x02 | TEST | 测试数据（RTT 测量） |
| 0x03 | RSSI | RSSI 信号强度 |

## 运行示例

将 `example.exe` 和 `lora_gateway_sdk.dll` 复制到 Windows 机器同一目录下运行：

```
LoRa Gateway SDK Example

Commands:
  s              - Search devices (UDP broadcast)
  c <ip> [port]  - Connect to gateway (default port 8899)
  d              - Disconnect
  n              - Get network parameters
  a <at_cmd>     - Send AT command (e.g. a AT+NINFO?)
  r              - Send test RSSI response
  q              - Quit

> s
[LOG] TX (192.168.1.10) -> USR1566...
[DEVICE] MAC=D4AD20ED63C4  Name=USR-LG210-L  SW=V1.0.1  IP=192.168.1.253
> c 192.168.1.253 8899
[CONN] CONNECTING
[CONN] CONNECTED
[FRAME] NID=00000001  len=9  type=0x01  -> Telemetry X=15 Y=-3 Btn=Released
```

## 许可证

Apache-2.0
