# LoRa Gateway Tool

连接 USR-LG210-L LoRa 网关的工具集，支持 TCP 数据帧收发和 UDP 设备配置。

提供两种工具：

- **Win32 GUI** (`lora_gateway_tool.exe`) — Windows 桌面应用
- **Python CLI** (`lora_tcp.py`) — 跨平台命令行工具
- **网关模拟器** (`lora_gateway_sim.py`) — 用于测试的模拟网关

## 功能

### TCP 数据通信

- 非阻塞连接网关，实时接收解析 LoRa 统一帧格式数据
- 数据帧类型分发: HANDLER (遥测) / TEST / RSSI / ACK
- 遥测数据实时显示 (X/Y 角度 + 按键状态)
- 自动 ACK (可开关) / 手动发送数据帧 / ACK / RSSI 查询
- 收发统计 (RX/TX/ERR)

### UDP 设备配置

- **设备发现**: 广播 SEARCH 消息 (USR1566 JSON, port 1566)
- **网络参数**: 查询 NETDEV (IP/Mask/GW)
- **AT 指令透传**: 查询 (`?`) 用 GETPARA, 设置 (`=`) 用 SETPARA
- **快捷命令**: Version / GWID / CSQ / DHCP / NWMODE / WMODE / CH / SPD / PWR 等

### Python CLI 独有功能

- **自动重连**: 远端断连后按间隔自动重试 (可开关, 默认开启)
- **手动重连**: `disconnect` 后可通过 `connect <ip> <port>` 重新连接
- **命令历史**: 上下键切换历史命令 (readline)
- **`--no-connect` 模式**: 启动时不自动连接

### 网关模拟器

- TCP Server: 模拟网关数据收发，支持自动/手动发送遥测、扫描仪、测试数据
- UDP Server: 模拟 SEARCH / GETPARA / SETPARA / AT 指令响应

## 构建 (Win32 GUI)

### 前置条件

- CMake >= 3.15
- Windows: MSVC (Visual Studio) 或 MinGW
- Linux 交叉编译: `x86_64-w64-mingw32-gcc`

### 编译

```shell
# Windows (MSVC)
cmake -B build && cmake --build build

# Windows (MSVC with VS developer command prompt)
cmake -B build -G "NMake Makefiles" && cmake --build build

# Linux 交叉编译 (MinGW)
cmake -B build \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres
cmake --build build
```

## 使用

### Win32 GUI

1. 启动 `lora_gateway_tool.exe`
2. **Data 页**: 输入网关 IP 和端口，点击 Connect
3. 连接成功后自动接收遥测数据，Auto ACK 默认开启
4. **Config 页**: 点击 Search Devices 发现局域网内网关
5. 发现后可查询网络参数、发送 AT 指令

### Python CLI

```shell
# 标准启动 (自动连接)
python3 lora_tcp.py <ip> <port> [--nid HEX_NID] [--no-auto-ack]

# 启动不自动连接
python3 lora_tcp.py --no-connect [--nid HEX_NID]

# 示例
python3 lora_tcp.py 192.168.2.100 1234 --nid 00000001
```

**命令列表:**

```
TCP:
  connect <ip> <port>       连接网关 (支持断连后重连)
  disconnect                断开连接 (停止自动重连)
  send <hex>                发送数据帧
  ack                       发送 ACK
  rssi                      发送 RSSI 查询
  telemetry                 发送遥测测试帧
  autoack [on|off]          切换自动 ACK
  nid [hex]                 设置/查询 NID 过滤
  prefix [hex]              设置/查询网关前缀

UDP:
  search                    搜索设备
  getnet                    获取网络参数
  at <cmd>                  发送 AT 指令
  ver / gwid / csq          快捷查询
  dhcp [on|off]             DHCP 开关
  ip / mask / gw [addr]     网络参数设置/查询
  option / nwmode / ttmode / wmode / upwid  LoRa 参数
  ch / spd / pwr <n> [val]  通道/速率/功率

自动重连:
  autorc [on|off]           切换自动重连 (默认 ON)
  retry [seconds]           设置重连间隔 (默认 3s)
  rmax [n]                  最大重连次数 (0=无限)
  status                    显示连接状态和统计

通用:
  help                      显示帮助
  quit                      退出
```

### 网关模拟器

```shell
python3 lora_gateway_sim.py [--tcp-port PORT] [--nid HEX] [--gwid HEX]
```

模拟器启动后监听 TCP/UDP，在 `sim>` 提示符下可手动发送模拟数据:

```
  telemetry [x] [y] [btn]   发送遥测帧
  scanner [id] <hex>         发送扫描仪数据
  test <hex>                 发送测试数据
  rssi                       发送 RSSI 请求
  auto [ms]                  自动发送遥测 (默认 500ms, 0=停止)
  stats                      显示统计
```

## 源码结构

```
tools/
  lora_gateway_tool.c   -- Win32 GUI: 控件创建、回调实现、窗口过程、WinMain
  lora_net.c            -- 共享层: 全局状态、endianness helpers、帧组帧、生命周期
  lora_net.h            -- 接口头文件: 类型、回调、extern 声明、API 声明
  lora_tcp.c            -- TCP: 连接管理、帧收发与解析、socket 事件分发
  lora_udp.c            -- UDP: 广播/单播、设备发现、AT 指令 (GETPARA/SETPARA)
  crc16.c / crc16.h     -- CRC16-CCITT 校验 (与 Zephyr crc16_ccitt 一致)
  cJSON.c / cJSON.h     -- JSON 解析库
  CMakeLists.txt        -- 构建配置
  lora_tcp.py           -- Python CLI: TCP + UDP 全功能命令行工具
  lora_gateway_sim.py   -- Python 网关模拟器: TCP Server + UDP Server
```

### 模块依赖 (C)

```
lora_gateway_tool.c (UI)
  ├── lora_net.h        → net_init/cleanup, 共享状态
  ├── lora_tcp.c API    → net_connect/disconnect/send_ack/send_data_frame/on_socket_event
  └── lora_udp.c API    → net_cfg_search/get_net/send/quick/on_udp_rx

lora_tcp.c (TCP)
  ├── lora_net.h        → net_ctx_t, callbacks, 共享状态
  └── crc16.h           → 帧校验

lora_udp.c (UDP)
  ├── lora_net.h        → net_ctx_t, callbacks, 设备信息状态
  └── cJSON.h           → JSON 构建/解析

lora_net.c (共享)
  ├── lora_net.h        → 接口实现
  └── crc16.h           → 帧组帧校验
```

TCP 与 UDP 模块之间零交叉依赖，仅通过 `lora_net.h` 共享回调接口和全局状态。

## 数据帧格式

### TCP 数据帧

```
TX (工具→网关): [Gateway Prefix 4B][0xAA][0x55][统一帧][\r\n]
RX (网关→工具): [0xAA][0x55][统一帧][\r\n]
```

### 统一帧格式

```
[NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE]
CRC 覆盖: NID + Length + Data (CRC 前所有字节)
```

### Data 首字节类型标识

| 类型值 | 名称 | 说明 |
|--------|------|------|
| 0x01 | HANDLER | 遥测数据 (8B: X/Y int16 BE + 按键 + 0xFF×3) |
| 0x02 | TEST | 测试数据 |
| 0x03 | RSSI | RSSI 信号强度请求/响应 |

### 遥测数据 (Data 8 字节, 大端序, 类型 0x01)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | coord_x | int16_t BE, 单位 0.1° |
| 2-3 | 2 | coord_y | int16_t BE, 单位 0.1° |
| 4 | 1 | btn flags | bit0: btnHandler(反转), 按下=0, 松开=1 |
| 5-7 | 3 | reserved | 固定 0xFF |

## UDP 配置协议

使用 USR1566 JSON 协议，通过 UDP 端口 1566 通信。

| 操作 | MSG 字段 | TYPE | CMD | 说明 |
|------|----------|------|-----|------|
| 搜索设备 | SEARCH | LORA | — | 广播发现局域网内网关 |
| 获取网络参数 | GETPARA | JSON | NETDEV | 查询 IP/子网掩码/网关 |
| 查询 AT 指令 | GETPARA | AT | `AT+XXX?` | 查询参数 (`?` 结尾) |
| 设置 AT 指令 | SETPARA | AT | `AT+XXX=val` | 设置参数 (`=` 赋值) |

**响应消息名:**
- SEARCH → ACK-SEARCH
- GETPARA → ACK-GETPARA
- SETPARA → ACK-SETPARA

## 许可证

Apache-2.0
