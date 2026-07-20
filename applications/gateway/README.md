# Gateway - 数据中转网关

运行在 STM32F103RCT6 上的 Zephyr RTOS 嵌入式应用，作为 mod_handler（手持控制器）与上位机之间的数据中转网关。**同一份代码支持两种板级配置**，通过 snippet 切换。

## 两种配置

| 配置 | 硬件 | 网络 | 用途 |
|------|------|------|------|
| **基线（默认）** | 手柄板（无 W5500） | 无 | 用手柄硬件充当无线接收端做功能测试，纯 nRF24 ↔ CAN 桥 |
| **网关板** | 网关板（带 W5500） | W5500 UDP | 完整网关，nRF24 ↔ CAN/UDP 双链路 |

两种配置共用同一份主干代码（nRF24 接收 + CAN 转发），差异由 Kconfig 开关 `CONFIG_GW_NETWORKING` 与 snippet `gateway_eth` 注入。

## 编译

```bash
# 手柄板（默认，无网络）—— 当前功能测试用
west build -b nrf24_f103rct6 applications/gateway --sysbuild

# 网关板（带 W5500 网络）
WITH_GATEWAY_ETH=1 west build -b nrf24_f103rct6 applications/gateway --sysbuild
```

> `WITH_GATEWAY_ETH` 是**环境变量**而非 `-D` 参数：sysbuild 不转发命令行 `-D` 给子镜像 app，故 snippet 启用通过环境变量在 app `CMakeLists.txt` 内追加（`if(DEFINED ENV{WITH_GATEWAY_ETH})`）。

烧录：`west flash`

## 硬件

### 共用（两种配置）

| 组件 | 型号 | 接口 |
|------|------|------|
| MCU | STM32F103RCT6 | - |
| 无线 | nRF24L01+ | SPI2 (8MHz) |

### 引脚分配

| 功能 | 手柄板（基线） | 网关板（+snippet） | 说明 |
|------|---------------|-------------------|------|
| SPI2 SCK/MISO/MOSI | PB13/PB14/PB15 | 同 | SPI 共享总线 |
| nRF24 CS / CE / IRQ | PB12 / PA9 / PC6 | 同 | - |
| CAN RX/TX | PA11/PA12 | 同 | 250Kbps |
| 主电源 (5V) | PA8 | PA8 | `mainpower`，软件使能 |
| CAN 电源 | PC7 | PC7 | `canpower`，软件使能 |
| nRF24 电源 | PC9 | PC9 | 驱动 `power-gpios` 管理 |
| Link Switch | PA10 | PA2 | 切换 CAN/UDP（网关板） |
| W5500 CS | — | PA10 | 仅网关板 |
| W5500 INT / RESET | — | PA1 / PB0 | 仅网关板 |

手柄板需软件使能三路电源（mainpower/canpower/rf24power），由 `main.c::gw_power_init()` 在 `PRE_KERNEL_2` 阶段拉高，确保 nRF24/CAN 芯片在驱动初始化前上电。

## 数据流

**手柄板（基线，仅 CAN）：**
- 上行: mod_handler → nRF24 → Gateway → CAN → 上位机
- 下行: 上位机 → CAN → Gateway → nRF24 → mod_handler

**网关板（+UDP，Link Switch 切换）：**
- CAN 模式: 同上
- UDP 模式: mod_handler → nRF24 → Gateway → UDP → 上位机（反向亦然）

## CAN 协议

### 数据转发帧

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x1E3 | 手柄→网关 | 手柄状态 (X/Y + 按键) |
| 0x263 | 网关→手柄 | 超欠挖 + 激光测距 |
| 0x363 | 网关→手柄 | X/Y 坐标 |
| 0x463 | 网关→手柄 | Z 坐标 |
| 0x763 | 手柄→网关 | 心跳 |

### nRF24 配置帧

| 帧 ID | 方向 | 格式 |
|-------|------|------|
| 0x104 | 平台→网关 | `[cmd 1B][param...]` |
| 0x105 | 网关→平台 | `[cmd 1B][channel 1B][addr 5B][reserved 1B]` |

### 网络配置帧（仅网关板）

| 帧 ID | 方向 | 格式 |
|-------|------|------|
| 0x106 | 平台→网关 | `[cmd 1B][data...]` |
| 0x107 | 网关→平台 | `[cmd 1B][ip 4B][reserved 3B]` |

### 固件升级帧（使用 libs/can_fw_upgrade）

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台→网关 | 控制命令 |
| 0x102 | 网关→平台 | 响应帧 |
| 0x103 | 平台→网关 | 固件数据 |

## UDP 协议（仅网关板 snippet 启用时）

### 数据帧格式
`[CAN ID 2B BE][payload]` (透传扫描仪数据)

### 命令帧格式
`[0xAA][0x55][cmd 1B][data...]` (2 字节魔数头区分命令和数据)

### 配置命令

| 命令 | 格式 | 说明 |
|------|------|------|
| 0x01 | `[0xAA][0x55][0x01][ip 4B]` | 设置 IP |
| 0x02 | `[0xAA][0x55][0x02][mask 4B]` | 设置子网掩码 |
| 0x03 | `[0xAA][0x55][0x03][gw 4B]` | 设置网关 |
| 0x04 | `[0xAA][0x55][0x04][port 2B BE]` | 设置 UDP 端口 |
| 0x05 | `[0xAA][0x55][0x05]` | 查询配置 |
| 0x06 | `[0xAA][0x55][0x06][mode 1B]` | 设置模式 (1=CAN, 2=UDP) |
| 0x07 | `[0xAA][0x55][0x07][channel 1B]` | 设置 RF24 信道 |
| 0x08 | `[0xAA][0x55][0x08]` | 重启设备 |

### 固件升级命令

| 命令 | 格式 | 说明 |
|------|------|------|
| 0x10 | `[0xAA][0x55][0x10]` | 开始固件升级 |
| 0x11 | `[0xAA][0x55][0x11][data...]` | 固件数据 (每包最大 256B) |
| 0x12 | `[0xAA][0x55][0x12]` | 结束固件升级并重启 |

## Shell 命令

```
link can       -- 切换到 CAN 模式
link udp       -- 切换到 UDP 模式（仅网关板，基线无此命令）
link status    -- 显示当前模式
```

## 资源占用

| 配置 | FLASH | RAM |
|------|-------|-----|
| 手柄板（基线） | 85 KB / 194 KB (43.9%) | 17.7 KB / 48 KB (36.0%) |
| 网关板（snippet） | 150 KB / 194 KB (77.4%) | 45.5 KB / 48 KB (92.7%) |

## 配置架构

- **Kconfig `CONFIG_GW_NETWORKING`**（`Kconfig`，默认 `n`）：C 代码编译开关。启用时 `select` 整套网络栈（`NETWORKING`/`NET_SOCKETS`/`POSIX_API`/`ETH_W5500`...），`udp_forward.c` 参与编译；关闭时网络相关代码全部经 `#ifdef` 排除。
- **snippet `gateway_eth`**（`snippets/gateway_eth/`）：启用网关板配置的"配方"。注入 `CONFIG_GW_NETWORKING=y`（conf）+ W5500 devicetree 节点 / PA10 CS / `chosen ethernet` / `linksw→PA2`（overlay），叠加在基线 board overlay 之上。
- **基线**（`prj.conf` + `boards/nrf24_f103rct6.overlay`）：手柄板最小集，无网络，默认编译即此配置。

主干转发代码（`rf24.c` / `can_forward.c`）两种配置零差异共享，网络分支全部由 `#ifdef CONFIG_GW_NETWORKING` 守卫。

## 目录结构

```
gateway/
  boards/
    nrf24_f103rct6.overlay  -- 基线板级覆盖 (手柄板: nRF24+CAN+电源, 无 W5500)
    nrf24_f103rct6.conf     -- 配对占位 (不可删, 内容可空)
  snippets/gateway_eth/      -- 网关板 snippet (W5500 网络)
    snippet.yml              -- append EXTRA_CONF_FILE + EXTRA_DTC_OVERLAY_FILE
    gateway_eth.conf         -- CONFIG_GW_NETWORKING=y + 随机源
    gateway_eth.overlay      -- W5500 节点 + PA10 CS + chosen ethernet + linksw PA2
  include/gateway.h          -- 公共定义
  src/
    main.c                   -- 入口 + 电源使能 + 链路切换 + (网络初始化)
    rf24.c                   -- nRF24L01P 收发
    can_forward.c            -- CAN 转发 + (网络配置命令)
    udp_forward.c            -- UDP 透传 + 配置 + 固件升级 (仅 CONFIG_GW_NETWORKING)
    config.c                 -- 配置管理
    persist.c                -- Settings 持久化
  CMakeLists.txt             -- SNIPPET_ROOT 注册 + WITH_GATEWAY_ETH 条件追加 snippet
  Kconfig                    -- CONFIG_GW_NETWORKING 开关
  prj.conf
  VERSION
  CLAUDE.md
  README.md
```
