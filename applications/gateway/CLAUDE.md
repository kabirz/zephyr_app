# CLAUDE.md -- gateway

## 项目愿景

gateway 是运行在 STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM) 上的 Zephyr RTOS 嵌入式应用，作为 **数据中转网关**，在 mod_handler（手持控制器）与上位机之间转发数据。

- **版本**: 0.1.0-dev
- **硬件平台**: nrf24_f103rct6 (网关板，独立 PCB)
- **Bootloader**: MCUBoot (swap-with-scratch 模式)
- **无线模块**: Nordic nRF24L01+ (SPI2，2.4GHz，中断驱动)
- **网络芯片**: Wiznet W5500 (SPI3 以太网)
- **许可证**: Apache-2.0

---

## 单配置架构

网关板硬件：nRF24L01+ (SPI2) ↔ W5500 以太网 (SPI3)，**无 CAN，无模块电源管理**。nRF24 接收到的数据通过 W5500 UDP 转发给上位机，反向亦然。

网络是唯一数据通路，配置直接写在 `prj.conf`，无需 snippet 或 Kconfig 开关切换。

---

## 编译

```shell
west build -b nrf24_f103rct6 applications/gateway --sysbuild
west flash
```

---

## 硬件引脚分配

| 功能 | 引脚 | 总线 | 说明 |
|------|------|------|------|
| nRF24 SCK/MISO/MOSI | PB13/PB14/PB15 | SPI2 | 8MHz |
| nRF24 CS / CE / IRQ | PB12 / PA9 / PC6 | SPI2 | nRF24L01P |
| W5500 SCK/MISO/MOSI | PB3/PB4/PB5 | SPI3 | 8MHz |
| W5500 CS | PA15 | SPI3 | W5500 片选 |
| W5500 INT / RST | PD2 / PC12 | — | 中断 / 复位 |

> **SPI3 JTAG 释放**：PB3/PB4/PA15 是 JTAG 引脚 (JTDO/JNTRST/JTDI)。Zephyr STM32 pinctrl 在使用 SPI3_REMAP0 时自动设置 `AFIO_MAPR_SWJ_CFG` 释放 JTAG（保留 SWD），无需手动 Kconfig。

> **无电源管理**：网关板所有模块常供电，无软件控制的电源使能 GPIO。

---

## 数据流

```
mod_handler --nRF24--> gateway --UDP--> host (上位机)
host       --UDP-->  gateway --nRF24--> mod_handler
```

---

## 帧协议

帧 ID 定义在 `gateway.h::enum can_ids`（复用历史 CAN 11-bit 编号，已与 CAN 总线无关，仅作逻辑标识符）：

| 帧 ID | 名称 | 方向 | 用途 |
|-------|------|------|------|
| 0x1E3 | `HANDLER_STATE` | 手柄→网关 | 手柄状态 (X/Y BE + 按键) |
| 0x263 | `OVERBREAK_LASER` | 网关→手柄 | 超欠挖 + 激光测距 |
| 0x363 | `COORD_XY` | 网关→手柄 | X/Y 坐标 |
| 0x463 | `COORD_Z` | 网关→手柄 | Z 坐标 |
| 0x763 | `COBID_HEATBEAT` | 手柄→网关 | 心跳 |
| 0x104 | `RF24_CONFIG_CMD` | 平台→网关 | nRF24 配置命令 |
| 0x105 | `RF24_CONFIG_RESP` | 网关→平台 | nRF24 配置响应 |
| 0x106 | `NET_CONFIG_CMD` | 平台→网关 | 网络配置命令 |
| 0x107 | `NET_CONFIG_RESP` | 网关→平台 | 网络配置响应 |
| 0x777 | `TEST_FRAME` | 双向 | rf24 shell 测试帧 (ping/echo/data) |

---

## UDP 协议

### 双端口架构

| 端口 | 用途 | 可配 | 默认 |
|------|------|------|------|
| **数据端口** | nRF24→上位机数据转发 + 上位机→nRF24 扫描仪数据透传 | `UDP_CMD_SET_PORT` 可改，持久化 | 9090 |
| **配置端口** | 所有配置命令（IP/掩码/网关/端口/RF24/重启/固件升级） | 固定 | 9200 |

- **双端口均设 `SO_BROADCAST`**：均支持广播收发。
- 配置端口绑定 `0.0.0.0:9200`，支持广播接收（上位机不知道设备 IP 时可广播配置）。
- **双端口发送策略一致**：发送方/目标与本机同子网 → 单播；跨子网 → 广播 `255.255.255.255:<端口>`。
  - 数据端口：`gw_udp_send` 按目标 IP 判断；未学习到同子网发送方前默认广播。
  - 配置端口：`config_send_resp` 按发送方 IP 判断。
- 两个 socket + 两个线程独立工作，配置流量不会劫持数据流量目标地址。

### 帧格式

- **数据帧**: `[帧 ID 2B BE][payload]`（透传扫描仪/手柄数据，走数据端口）
- **命令帧**: `[cmd 1B][data...]`（无魔数头，走配置端口）
- 配置命令 0x01~0x09（IP/掩码/网关/数据端口/查询/RF24 信道/RF24 地址/重启）
- 固件升级命令 0x10~0x12（开始/数据/结束重启）

---

## 关键设计决策

- **单一网络通路**：无 CAN，无模式切换 (linksw)，网络 (W5500 UDP) 是唯一数据通路。`connect_type` 字段已移除。
- **网络配置直写 prj.conf**：`CONFIG_NETWORKING`/`NET_IPV4`/`NET_UDP`/`NET_SOCKETS`/`POSIX_API`/`NET_L2_ETHERNET`/`NET_ARP` 直接写在 `prj.conf`。`ETH_DRIVER` 由 `NET_L2_ETHERNET` 自动拉起，`ETH_W5500` 由 devicetree W5500 节点自动拉起。
- **SPI 分离**：nRF24 用 SPI2，W5500 用 SPI3，各自独立 CS，避免总线共享。
- **STM32F103 无硬件 RNG**：网络栈随机源用 `CONFIG_TEST_RANDOM_GENERATOR`。
- **固件升级走 UDP**：`udp.c` 内置固件升级命令 (0x10~0x12)，通过 UDP 接收固件写入 slot1。
- **帧 ID 复用历史编号**：`enum can_ids` 保留原 CAN 11-bit 编号作为 UDP/nRF24 帧的逻辑标识符，上位机协议兼容。

---

## 源码结构

```
gateway/
  boards/
    nrf24_f103rct6.overlay   -- 板级覆盖（nRF24 SPI2 + W5500 SPI3 + RTC）
  include/gateway.h          -- 公共定义（帧 ID 枚举、配置参数、gateway_params_t）
  src/
    main.c                   -- 入口 + 网络初始化 (W5500 静态 IP)
    rf24.c                   -- nRF24L01P 收发（接收数据无条件走 UDP）
    rf24_shell.c             -- rf24 shell 测试命令（info/ch/addr/send/ping/listen/diag）
    udp.c                    -- UDP 透传 + 配置 + 固件升级
    config.c                 -- 配置加载（settings_load 封装）
    persist.c                -- Settings 持久化（gw/* 键，FCB 后端）
  CMakeLists.txt             -- 源文件列表
  Kconfig                    -- 线程栈/优先级配置
  prj.conf                   -- 应用配置（含网络栈）
  sysbuild.conf              -- MCUBoot sysbuild
  sysbuild/mcuboot.conf
  VERSION
```

### 线程清单

| 线程名 | 函数 | 栈/优先级 | 说明 |
|--------|------|-----------|------|
| `thread_rf24_rx` | `rf24_rx_thread` | 1024 / 8 | nRF24 接收，转发到数据端口 |
| `thread_udp_data_rx` | `udp_data_rx_thread` | 1024 / 10 | 数据端口 (9090) 接收，扫描仪数据透传到 nRF24 |
| `thread_udp_config_rx` | `udp_config_rx_thread` | 1024 / 10 | 配置端口 (9200) 接收，命令/固件升级分发 |

---

## AI 使用指引

- **无 CAN、无电源管理、无模式切换**：历史的双配置/CAN/snippet/linksw/电源管理代码已全部移除。
- **修改引脚**：全部在 `boards/nrf24_f103rct6.overlay`。
- **网络配置**：直接改 `prj.conf`，不涉及 Kconfig 开关。
- **UDP 双端口**：数据端口 (默认 9090, 可配) + 配置端口 (固定 9200, 支持广播)。改端口分工看 `udp.c` 头注释。
- **修改帧协议**：同步更新 `gateway.h::enum can_ids`，并与 mod_handler 保持一致（共用协议）。
- **`gateway_params_t gw_params`**（定义在 `main.c`）是全局共享状态：RF24 配置、网络配置、运行标志。
- 与 mod_handler 的关系：mod_handler 是手柄端（采集+发送），gateway 是接收/中转端，通过 nRF24 无线连接。
