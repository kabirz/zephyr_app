# LoRa Gateway Tool

Windows 桌面工具，用于连接 USR-LG210-L LoRa 网关，实时接收/解析 LoRa 遥测数据帧，并通过 UDP 广播配置网关参数。

## 功能

- **TCP 数据流**: 非阻塞连接网关，实时接收解析 LoRa 统一帧格式数据
- **遥测显示**: 实时显示 X/Y 角度、按键状态、收发计数
- **原始日志**: Hex 格式数据收发日志
- **手动发送**: 支持手动发送自定义数据帧和 ACK
- **历史记录**: ListView 记录所有收发事件
- **设备发现**: UDP 广播搜索局域网内的 LoRa 网关
- **网络参数**: 查询网关 IP/子网掩码/网关/GWID/信号强度
- **AT 指令**: 通过 UDP 透传 AT 指令配置 LoRa 模块

## 构建

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

1. 启动 `lora_gateway_tool.exe`
2. **Data 页**: 输入网关 IP 和端口，点击 Connect
3. 连接成功后自动接收遥测数据，Auto ACK 默认开启
4. **Config 页**: 点击 Search Devices 发现局域网内网关
5. 发现后可查询网络参数、发送 AT 指令

## 源码结构

```
tools/
  lora_gateway_tool.c   -- Win32 GUI: 控件创建、回调实现、窗口过程、WinMain
  lora_net.c            -- 共享层: 全局状态、endianness helpers、帧组帧、生命周期
  lora_net.h            -- 接口头文件: 类型、回调、extern 声明、API 声明
  lora_tcp.c            -- TCP: 连接管理、帧收发与解析、socket 事件分发
  lora_udp.c            -- UDP: 广播/单播、设备发现、AT 指令、响应处理
  crc16.c / crc16.h     -- CRC16-CCITT 校验 (与 Zephyr crc16_ccitt 一致)
  cJSON.c / cJSON.h     -- JSON 解析库
  CMakeLists.txt        -- 构建配置
```

### 模块依赖

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

## LoRa 统一帧格式

```
[NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE]
CRC 覆盖: NID + Length + Data (CRC 前所有字节)
```

| 帧类型 | 方向 | Data 内容 | 总长度 |
|--------|------|-----------|--------|
| 遥测/心跳 | 手柄→网关 | 8 字节 (同 CAN 0x1E3) | 16 字节 |
| 心跳 ACK | 网关→手柄 | 0 字节 (空) | 8 字节 |
| 扫描仪数据 | 网关→手柄 | 2B CAN ID BE + CAN data | 变长 |

### 遥测数据 (Data 8 字节, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | coord_x | int16_t BE, 单位 0.1° |
| 2-3 | 2 | coord_y | int16_t BE, 单位 0.1° |
| 4 | 1 | btn flags | bit0: btnHandler(反转), 按下=0, 松开=1 |
| 5-7 | 3 | reserved | 固定 0xFF |

## UDP 配置协议

使用 USR1566 JSON 协议，通过 UDP 端口 1566 通信。

| 操作 | MSG 字段 | 说明 |
|------|----------|------|
| 搜索设备 | SEARCH | 广播发现局域网内网关 |
| 获取网络参数 | GETPARA (CMD=NETDEV) | 查询 IP/子网掩码/网关 |
| 发送 AT 指令 | GETPARA (TYPE=AT) | 透传 AT 指令到 LoRa 模块 |

## 许可证

Apache-2.0
