# CLAUDE.md -- mod_handler

## 项目愿景

mod_handler 是一个运行在 STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM) 上的 Zephyr RTOS 嵌入式应用。它作为激光测距系统的**手持控制器模块**，负责采集操纵杆角度、电池状态，并通过 CAN 总线或 LoRa 无线与激光设备通信，同时提供 OLED 显示和 OTA 固件升级能力。

- **版本**: 0.1.2-release
- **硬件平台**: lora_f103rct6 (自定义板)
- **Bootloader**: MCUBoot (swap-with-scratch 模式)
- **LoRa 模块**: 有人物联网 WH-L101-L (串口透传, UART AT 指令)
- **LoRa 网关**: 有人物联网 USR-LG210-L
- **许可证**: Apache-2.0

---

## 架构总览

系统采用 Zephyr 多线程架构，通过 `gloval_params_t` 全局结构体在模块间共享状态，使用 `k_event` 进行跨线程事件通知。外围设备电源由 GPIO 独立控制，支持电源管理回调。

```
main() + SYS_INIT
  |
  +-- 主循环 (main thread)
  |     +-- 启动时初始全屏刷新, 之后事件驱动
  |
  +-- CAN 总线 (mod_can_process_thread, priority 11)
  |     +-- 固件升级协议 (fw_update)
  |     +-- 心跳发送 (mod_can_thread, priority 11)
  |     +-- 手柄状态上报: 心跳成功时发送 0x1E3 帧 (X/Y BE + 按键反转 + 0xFF)
  |     +-- 扫描仪数据接收: 0x263/0x363/0x463 (即时解析+显示)
  |     +-- LoRa 远程配参: 0x105/0x106 (k_work 异步, 不阻塞 CAN 线程)
  |     +-- 心跳失败 3 次 → connect_type = LORA_TYPE
  |
  +-- LoRa UART (WH-L101-L, UART async + DMA, lora_msg_process_thread, priority 12)
  |     +-- 数据模式: DMA 双缓冲透传, rx_timeout=20ms 判定帧边界
  |     +-- 统一帧格式: [NID 4B BE][Length 2B BE][Data NB][CRC16 2B BE]
  |     +-- 遥测发送: 8 字节帧 (角度/按键), 事件驱动 (仅 CAN 断开时)
  |     +-- 接收解析: ACK 通知 lora_ack_sem + 扫描仪数据复用 mod_can_parse_scanner
  |     +-- AT 模式: +++a 握手进入, 同步指令, 经 USR-LG210-L 网关中转
  |     +-- 网关管理: FP/TRANS/NET 三种模式, PROT 选择 NODE/LG210/LG220
  |
  +-- LoRa 心跳 (lora_heartbeat_thread, priority 12)
  |     +-- 仅 connect_type == LORA_TYPE 时运行
  |     +-- 2s 周期发送遥测帧, 等待网关 ACK (1.5s 超时)
  |     +-- 连续 3 次失败 → lora_connected = false
  |
  +-- ADC 操纵杆 (adc_read_thread, priority 7)
  |     +-- X/Y 角度采集 → 500ms, 变化时即时显示 + CAN/LoRa 发送
  |     +-- 电源电压采集 → 5s 周期, 变化时即时显示
  |
  +-- 电池 & 按键 (battery_monitor_thread, priority 7)
  |     +-- 充电状态检测 + 按键中断 (ISR → k_work → 即时显示+发送)
  |
  +-- OLED 显示 (SH1106 128x64, I2C, 事件驱动)
  |     +-- Row 0: 连接类型 (CAN/LORA) + 24x16 水平电池图标
  |     +-- Row 1: 激光距离 (D:) + 超欠挖 (OB:) (扫描仪 CAN/LoRa 数据, 即时刷新)
  |     +-- Row 2: X/Y 轴角度 (变化时即时刷新)
  |     +-- Row 3: 按键状态 (ISR 触发即时刷新)
  |
  +-- 电源管理 (can_power_init, PRE_KERNEL_2)
        +-- 4 路 GPIO 电源开关 + PM notifier
```

### 关键设计决策

- **全局状态共享**: `gloval_params_t global_params` (定义在 `common.h`) 是所有模块共享的中心状态结构，包含操纵杆角度、电池状态、扫描仪数据 (`scanner_data_t`)、事件对象等。
- **事件驱动显示**: 启动时全屏刷新一次，之后各模块在数据变化时即时更新对应 OLED 行。`display_str_pad` 渲染字符串后用空格填充行尾，防止旧数据残留。显示函数接口拆分为细粒度参数（如 `mod_display_handler_xy(int x, int y)` 而非 `gloval_params_t*`），便于跨模块直接调用。
- **心跳保活机制**: CAN 心跳线程在发送失败 3 次后将 `connect_type` 切换为 `LORA_TYPE`，直到 `TIMEOUT_EVENT` 重新触发。心跳成功时设置 `connect_type = CAN_TYPE` 并发送手柄状态帧 (0x1E3, 大端序)。连接类型切换时即时刷新 OLED Row 0。
- **OTA 通过 CAN**: 固件升级完全通过 CAN 总线传输，分两阶段 -- 控制帧走 `PLATFORM_RX` (0x103)，数据帧走 `FW_DATA_RX` (0x103)。使用 MCUBoot swap-with-scratch 确保掉电安全。
- **外设独立供电**: CAN、LoRa、显示屏、5V 电源各有独立 GPIO 使能引脚，由 `power.c` 统一管理。
- **线程间通信**: CAN 和 LoRa 各自使用 `k_msgq`，心跳使用 `k_sem` + `k_event` 组合实现超时检测。
- **LoRa 双模式驱动**: `lora.c` 基于 UART async API + DMA 实现。数据模式使用 DMA 双缓冲 + `rx_timeout=20ms` 判定帧边界；AT 模式通过 `+++` → `a` → `+OK` 握手进入，支持同步指令收发。`lora_mode_mutex` 保护 AT 模式切换，`lora_tx_mutex` 保护多线程 TX 竞争。
- **LoRa 统一帧格式**: 所有收发数据使用统一二进制帧 `[NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE]`。`lora_data_send()` 自动组帧，`parse_lora_frame()` 统一解析。常量定义在 `lora.h`: `LORA_FRAME_OVERHEAD=8`, `LORA_FRAME_HEADER_SIZE=6`。
- **LoRa 遥测协议**: Data 8 字节 (与 CAN 0x1E3 一致: X/Y int16_t BE + 按键反转 + 0xFF)，事件驱动发送。角度或按键变化时，若 `connect_type != CAN_TYPE` 则通过 LoRa 发送。
- **LoRa 接收数据**: 网关下发的扫描仪数据与 CAN 帧格式一致，Data 字段 = `[CAN frame ID 2B BE][CAN data NB]`，复用 `mod_can_parse_scanner()` 解析，自动触发显示刷新。空载荷为心跳 ACK。
- **LoRa 链路检测**: 应用层心跳，仅 `connect_type == LORA_TYPE` 时由 `lora_heartbeat_thread` 维护。2s 周期发遥测帧，1.5s ACK 超时，连续 3 次失败 → `lora_connected = false`。收到合法帧 (NID 匹配 + CRC 正确) → `lora_connected = true`。`lora_is_connected()` 供外部查询。ACK 通过 `lora_ack_sem` (k_sem) 在接收线程和心跳线程间通知。
- **CAN 手柄状态协议**: 帧 ID 0x1E3 (`HANDLER_STATE`)，8 字节 payload (X/Y int16 BE + 按键反转 + 0xFF 保留)，由心跳线程在心跳成功时随周期发送，同时在角度/按键变化时由 ADC/按键模块即时发送。所有多字节数据使用大端序 (网络字节序)，按键 btnHandler 反转 (按下=0, 松开=1)。
- **CAN 扫描仪数据接收**: 帧 ID 0x263 (`OVERBREAK_LASER`), 0x363 (`COORD_XY`), 0x463 (`COORD_Z`)，接收扫描仪下发的超欠挖+激光测距、X/Y/Z 坐标数据。解析后存入 `global_params.scanner` 并即时触发显示刷新。Z 坐标有效标志同时设置 X/Y/Z 三者的有效性。
- **CAN/LoRa 互斥**: 默认 `connect_type = CAN_TYPE` (在 `main_init` 中设置)。CAN 心跳成功 → 保持 CAN_TYPE + 发手柄状态帧；心跳 3 次失败 → 切换 LORA_TYPE，角度/按键变化时通过 LoRa 发送；CAN 恢复 → 自动切回。连接类型切换即时刷新 OLED。
- **LoRa 网关管理**: 支持三种通信协议 (`LORA_PROT_NODE`/`LORA_PROT_LG210`/`LORA_PROT_LG220`, AT+LORAPROT) 和三种工作模式 (`LORA_GW_MODE_FP` 点对点 / `LORA_GW_MODE_TRANS` 透传 / `LORA_GW_MODE_NETWORK` 组网, AT+WMODE)。透传/点对点模式仅需 SPD+CH，组网模式额外需要 GWID。默认 prot=LG210, mode=TRANS。GWID 和 NID 通过独立的 AT 指令 (AT+GWID/AT+NID) 设置，与通信参数 (prot/mode/spd/ch) 分离管理。
- **CAN 远程配参**: Host 通过 CAN 帧 `LORA_CONFIG_RX` (0x105) 发送 SET/QUERY/QUERY_NID/SET_NID/QUERY_GWID/SET_GWID 命令，设备通过 `LORA_CONFIG_TX` (0x106) 响应。0x105 SET 格式: data[0]=0x01(SET), data[1]=prot[7:4]|mode[3:0], data[2]=SPD, data[3]=CH (不再包含 GWID)。0x105 QUERY: data[0]=0x02。0x105 QUERY_NID: data[0]=0x03。0x105 SET_NID: data[0]=0x04, data[4-7]=NID(BE)。0x105 QUERY_GWID: data[0]=0x05。0x105 SET_GWID: data[0]=0x06, data[4-7]=GWID(BE)。所有多字节字段使用大端序。配置操作耗时 10s+（AT 模式握手 + 模块重启），使用 `k_work` 异步执行，不阻塞 CAN 接收线程。命令/结果枚举定义在 `mod-can.h`。
- **OLED 显示**: 事件驱动刷新。启动时 `mod_display_all` 全屏刷新，之后各模块在数据变化时调用对应行刷新函数。`display_str_pad` 渲染后用空格填充行尾清除旧数据残留。所有 `display_write` 通过 `display_mutex` 保护。显示函数接口为细粒度参数（如 `mod_display_handler_xy(int x, int y)`），不依赖完整 `gloval_params_t` 指针。
- **ADC 数据流**: 500ms 周期采集 X/Y 角度，变化时即时显示 + CAN/LoRa 发送。电量每 10 个周期 (5s) 采集一次，变化时即时显示。
- **按键事件流**: GPIO ISR 检测按键变化 → `k_work_submit` → 工作队列线程中即时显示按键状态 + CAN/LoRa 发送。ISR 中不做显示/发送操作。
- **连接类型**: `connect_type` 字段区分 `CAN_TYPE (1)` 和 `LORA_TYPE (2)`，默认 CAN。切换时即时刷新 OLED Row 0。
- **Shell 调试**: `lora send/at/exit` 基础命令 + `lora gw config [prot] [mode] [spd] [ch]` 通信参数配置 + `lora gw query` 通信参数查询 + `lora nid [hex]` 查询/设置节点 ID + `lora gwid [hex]` 查询/设置网关 ID。

---

## 构建与开发

### 构建

```shell
# 标准构建（带 MCUBoot sysbuild）
west build -b lora_f103rct6 . --sysbuild

# 带 shell 和 imgmgr snippet
west build -b lora_f103rct6 . --sysbuild -Dmod_handler_SNIPPET=imgmgr-shell
```

### 烧录

```shell
west flash
```

### 清理重建

```shell
west build -b lora_f103rct6 . --sysbuild --pristine
```

### 硬件连接 (devicetree overlay)

板级 overlay 定义在 `boards/lora_f103rct6.overlay`，核心引脚分配:

| 功能 | 引脚 | 说明 |
|------|------|------|
| CAN 电源 | PC7 | GPIO 输出 |
| 显示电源 | PC8 | GPIO 输出 |
| LoRa 电源 | PC9 | GPIO 输出 |
| 5V 使能 | PA8 | GPIO 输出 |
| LoRa HOSTWAKE | PA0 | GPIO 输入 (模块忙状态) |
| LoRa RESET | PB3 | GPIO 输出 |
| 操纵手柄按键 | PB0 | GPIO 输入 |
| 充电满 | PB12 | GPIO 输入上拉 (低有效) |
| 充电中 | PB13 | GPIO 输入上拉 (低有效) |
| 电源键 | PB14 | GPIO 输入上拉 |
| ADC-X | PC4 (ADC1_CH14) | 操纵杆 X 轴 |
| ADC-Y | PC5 (ADC1_CH15) | 操纵杆 Y 轴 |
| ADC-VCC | PB1 (ADC1_CH9) | 电源电压采样 |
| CAN | PA11/PA12 | CAN1, 250Kbps |
| LoRa UART | PA2/PA3 | USART2, 115200bps (WH-L101-L, DMA1 CH6/CH7) |
| 调试串口 | PB6/PB7 | USART1 (remap), 115200bps |
| OLED I2C | PB10/PB11 | I2C2, SH1106 @ 0x3C |
| SPI Flash | PA5/PA6/PA7/PA4 | SPI1, GD25Q80 (存放升级镜像) |

---

## CAN 通信协议

帧 ID 定义在 `mod-can.h`:

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 (`PLATFORM_RX`) | 平台->手柄 | 控制命令 (升级/确认/版本/重启) |
| 0x102 (`PLATFORM_TX`) | 手柄->平台 | 响应帧 |
| 0x103 (`FW_DATA_RX`) | 平台->手柄 | 固件数据传输 |
| 0x763 (`COBID_HEATBEAT`) | 手柄->平台 | 心跳 (400ms 周期) |
| 0x1E3 (`HANDLER_STATE`) | 手柄->平台 | 手柄状态 (X/Y BE + 按键反转) |
| 0x263 (`OVERBREAK_LASER`) | 平台->手柄 | 超欠挖 + 激光测距 (BE) |
| 0x363 (`COORD_XY`) | 平台->手柄 | X/Y 坐标 (BE) |
| 0x463 (`COORD_Z`) | 平台->手柄 | Z 坐标 (BE) |
| 0x105 (`LORA_CONFIG_RX`) | 平台->手柄 | LoRa 参数设置/查询命令 |
| 0x106 (`LORA_CONFIG_TX`) | 手柄->平台 | LoRa 参数配置/查询响应 |

### CAN LoRa 配参帧格式 (0x105/0x106, DLC 8, 所有命令/响应)

所有多字节字段使用大端序 (BE)。GWID/NID 与通信参数 (prot/mode/spd/ch) 独立管理。

**命令码:**

| 命令码 | 名称 | 说明 |
|--------|------|------|
| 0x01 | SET | 设置通信参数 (prot/mode/spd/ch) |
| 0x02 | QUERY | 查询通信参数 |
| 0x03 | QUERY_NID | 查询节点 ID |
| 0x04 | SET_NID | 设置节点 ID |
| 0x05 | QUERY_GWID | 查询网关 ID |
| 0x06 | SET_GWID | 设置网关 ID |

**SET 通信参数 (0x105, data[0] = 0x01, DLC 8):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 命令 | 0x01 = SET |
| 1 | 1 | prot[7:4] + mode[3:0] | prot: 0=NODE, 1=LG210, 2=LG220; mode: 0=FP, 1=TRANS, 2=NET |
| 2 | 1 | SPD | 速率等级 1-12 |
| 3 | 1 | CH | 信道 0-127 |

**SET/QUERY 响应 (0x106):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 结果 | 0x00=成功, 0x01=失败 |
| 1 | 1 | prot[7:4] + mode[3:0] | 同 SET 格式 |
| 2 | 1 | SPD | 速率等级 |
| 3 | 1 | CH | 信道 |

**QUERY_NID (0x03) / SET_NID (0x04) / QUERY_GWID (0x05) / SET_GWID (0x06):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 命令 | 命令码 |
| 4-7 | 4 | NID/GWID | uint32_t BE (仅 SET 命令携带) |

**NID/GWID 响应 (0x106, DLC 8):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 结果 | 0x00=成功, 0x01=失败 |
| 4-7 | 4 | NID/GWID | uint32_t BE |

## LoRa 统一帧格式

所有 LoRa 收发数据使用统一二进制帧格式:

```
┌──────────┬──────────┬──────────────┬──────────┐
│ NID 4B   │ Length 2B│ Data NB      │ CRC16 2B │
│ uint32_t │ uint16_t│ 变长         │ CCITT    │
│ BE       │ BE      │              │ BE       │
└──────────┴──────────┴──────────────┴──────────┘
CRC 覆盖: NID + Length + Data (CRC 前所有字节)
```

| 帧类型 | 方向 | Data 内容 | 总长度 |
|--------|------|-----------|--------|
| 遥测/心跳 | 手柄→网关 | 8 字节 (同 CAN 0x1E3) | 16 字节 |
| 心跳 ACK | 网关→手柄 | 0 字节 (空) | 8 字节 |
| 扫描仪数据 | 网关→手柄 | 2B CAN ID BE + CAN data | 变长 |

### LoRa 遥测数据 (Data 8 字节, 大端序, 与 CAN 0x1E3 一致)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | coord_x | int16_t BE, 单位 0.1° |
| 2-3 | 2 | coord_y | int16_t BE, 单位 0.1° |
| 4 | 1 | btn flags | bit0: btnHandler(反转), 按下=0, 松开=1 |
| 5-7 | 3 | reserved | 固定 0xFF |

### LoRa 扫描仪数据 (网关→手柄)

Data 字段 = `[CAN frame ID 2B BE][CAN data bytes]`，与 CAN 帧数据一致，复用 `mod_can_parse_scanner()` 解析。

### LoRa 链路检测

- 心跳线程仅 `connect_type == LORA_TYPE` 时运行
- 每 2s 发送遥测帧，等待网关 ACK (空载荷帧)，超时 1.5s
- 连续 3 次超时 → `lora_connected = false`
- 收到任何合法帧 (NID 匹配 + CRC 正确) → `lora_connected = true`
- `lora_is_connected()` 供外部查询链路状态
- ACK 通知: 接收线程 `k_sem_give(&lora_ack_sem)` → 心跳线程 `k_sem_take()`

### LoRa 网关参数

| 参数 | AT 指令 | 说明 |
|------|---------|------|
| 协议 | AT+LORAPROT | NODE (点对点), LG210 (默认), LG220 |
| 工作模式 | AT+WMODE | FP (点对点), TRANS (透传, 默认), NET (组网) |
| 速率 | AT+SPD | 1-12, 默认 10 |
| 信道 | AT+CH | 0-127, 默认 72 (470MHz) |
| 网关 ID | AT+GWID | uint32_t, 组网模式有效 |
| 节点 ID | AT+NID | uint32_t |

### LoRa Shell 命令

```
lora send <data>                           -- 透传模式发送
lora at <cmd>                              -- 发送 AT 指令 (自动进入 AT 模式)
lora exit                                  -- 退出 AT 模式
lora gw config [prot] [mode] [spd] [ch]   -- 配置通信参数
  prot: node, lg210 (default), lg220
  mode: trans (default), fp, net
lora gw query                              -- 查询当前通信参数
lora nid [hex_value]                       -- 查询/设置节点 ID
lora gwid [hex_value]                      -- 查询/设置网关 ID
```

---

## 源码结构

```
include/
  common.h          -- 全局状态类型定义 (gloval_params_t, scanner_data_t, CAN_TYPE/LORA_TYPE, nid/gwid/rssi)
  mod-can.h         -- CAN 协议定义 + 固件升级接口 + LoRa 配参命令/结果枚举
  lora.h            -- LoRa 驱动接口 (透传/AT/遥测帧/网关配置/NID-GWID管理/链路检测/统一帧常量)
  display.h         -- 显示模块接口 (font8x16_t 类型定义)
  power.h           -- 外设电源控制接口
src/
  main.c            -- 入口 + 初始显示
  can.c             -- CAN 收发 + 心跳线程 + 手柄状态上报 (0x1E3 BE) + 扫描仪数据解析+显示 + LoRa 远程配参 (k_work)
  firmware.c        -- OTA 固件升级状态机
  lora.c            -- LoRa UART async+DMA + 统一帧收发 + 遥测 (事件驱动) + 接收数据分发 + 心跳线程 + 网关管理 + Shell
  font_8x16.c       -- 8x16 ASCII 字体位图 (95 字符, 1520 字节)
  adc.c             -- ADC 操纵杆角度采集 + 电量映射 (变化时即时显示+发送)
  battery.c         -- 电池/按键 GPIO 监测 (ISR → k_work 即时显示+发送)
  display.c         -- SH1106 OLED 显示 (8x16 文本 + 电池图标, display_str_pad 防残留)
  power.c           -- 外设电源管理 + PM notifier
boards/
  lora_f103rct6.overlay  -- 板级 devicetree 覆盖
sysbuild.conf            -- MCUBoot 配置
sysbuild/mcuboot.conf    -- MCUBoot 参数
VERSION                  -- 版本号 (0.1.2-release)
```

---

## 线程清单

| 线程名 | 函数 | 栈大小 | 优先级 | 说明 |
|--------|------|--------|--------|------|
| `mod_can` | `mod_can_process_thread` | 2048 | 11 | CAN 消息接收与分发 |
| `can_heart` | `mod_can_thread` | 1024 | 11 | CAN 心跳保活 + 手柄状态周期上报 |
| `lora_msg` | `lora_msg_process_thread` | 1024 | 12 | LoRa 串口消息处理 + 统一帧解析 + 扫描仪数据分发 |
| `lora_heart` | `lora_heartbeat_thread` | 1024 | 12 | LoRa 心跳 (仅 LORA_TYPE 时运行, 2s 周期 + ACK 检测) |
| `adc_thread_id` | `adc_read_thread` | 1024 | 7 | ADC 周期采集 (500ms, 变化时即时发送) |
| `battery_monitor_tid` | `battery_monitor_thread` | 1024 | 7 | 电池状态监测 (5s) |

---

## 编码规范

- 使用 Zephyr LOG 模块 (`LOG_MODULE_REGISTER`, `LOG_INF/ERR/DBG/WRN`)
- 设备获取使用 devicetree 宏 (`DEVICE_DT_GET`, `GPIO_DT_SPEC_GET`)
- 线程使用 `K_THREAD_DEFINE` 静态定义
- 初始化使用 `SYS_INIT` 分级初始化
- GPIO 引脚全部通过 `zephyr,user` devicetree 节点定义
- 头文件保护使用 `#ifndef _MOD_XXX_H__` 或 `#ifndef __MOD_XXX_H__` 模式
- 显示为事件驱动：各模块数据变化时直接调用对应行刷新函数，不在主循环轮询刷新
- 所有 display_write 调用通过 `display_mutex` 保护
- 显示函数使用 `display_str_pad` 替代 `display_str`，空格填充行尾防止旧数据残留
- 显示函数接口为细粒度参数（如 `mod_display_handler_xy(int x, int y)`），不依赖完整 `gloval_params_t*`
- ISR 中通过 `k_work_submit` 提交任务，不在中断上下文做显示/发送操作

---

## AI 使用指引

- 修改 CAN 协议时，同步更新 `mod-can.h` 中的枚举定义
- 添加新外设时，先在 `boards/lora_f103rct6.overlay` 的 `zephyr,user` 节点中定义引脚，然后在对应驱动中用 `GPIO_DT_SPEC_GET` / `ADC_DT_SPEC_GET_BY_IDX` 获取
- 电源管理相关代码目前被注释掉 (`CONFIG_PM` 未启用)，如需启用需在 `prj.conf` 中取消注释
- `gloval_params_t` 中 `connect_type` 使用 `CAN_TYPE (1)` / `LORA_TYPE (2)` 区分连接类型
- Flash 分区: 内部 Flash 仅存放 mcuboot(64KB) + image-0(192KB)，image-1 和 scratch 在外部 SPI Flash (GD25Q80)
- `main.c` 中的 `#ifndef CONFIG_FLASH_SIZE` 保护是针对 native_sim 构建的兼容处理
- 字体/图标位图取模方式: MONO_VTILED, MSB 上高位, 与 SH1106 原生格式一致
- LoRa AT 指令格式参考 `doc/` 目录下的 WH-L101-L AT 指令集和 LG210 协议说明书
- 修改遥测帧格式时，需同步更新 `lora.h` 中的帧格式注释和常量、`lora.c` 中的统一帧打包/解析逻辑、`mod-can.h` 和 `can.c` 中的手柄状态逻辑
- CAN 帧使用大端序 (网络字节序): 发送用 `sys_put_be16`/`sys_put_be32`，接收用 `sys_get_be16`/`sys_get_be32`
- CAN/LoRa 互斥通过 `global_params.connect_type` 控制: CAN 心跳线程写、ADC/按键模块读，无需额外锁 (atomic 级别的 byte 写入)
- 扫描仪数据通过 `global_params.scanner` (scanner_data_t) 存储，CAN 接收线程或 LoRa 接收线程写入并即时触发显示刷新
- CAN 远程配参 (0x105/0x106) 使用 `k_work` 异步处理: CAN 接收线程解析参数后 `k_work_submit()`，工作队列执行 `lora_gw_configure()` / `lora_gw_query()` 完成后发送响应帧
- CAN 0x105 SET 帧格式: data[1] 高 4 位 = prot, 低 4 位 = mode, data[2]=SPD, data[3]=CH (不含 GWID)。NID/GWID 独立命令: QUERY_NID(0x03)/SET_NID(0x04), QUERY_GWID(0x05)/SET_GWID(0x06)。NID/GWID 数据使用 BE (sys_put_be32/sys_get_be32)。SET_NID/SET_GWID 需 AT 模式写模块并重启, 通过 k_work 异步执行
- `lora_cfg_work`、`pending_lora_cfg`、`pending_nid`、`pending_gwid`、`lora_cfg_cmd` 为 `can.c` 内部 static 变量，由 CAN 接收线程写入、系统工作队列读取，单生产者单消费者无需锁
- `btn_display_work` 为 `battery.c` 内部 static 变量，ISR 中 `k_work_submit`，工作队列线程执行显示+发送
- LoRa TX 线程安全: `lora_tx_mutex` 保护 `lora_data_send()`，防止 ADC 线程、按键 k_work、心跳线程并发 TX 竞争
- LoRa 链路状态: `lora_connected` 为 `atomic_t`，接收线程置 true，心跳线程连续超时置 false，`lora_is_connected()` 供外部无锁读取
- LoRa ACK 通知: `lora_ack_sem` (k_sem, max=1)，接收线程解析到空载荷帧时 `k_sem_give()`，心跳线程 `k_sem_take()` 等待 ACK
- LoRa 接收线程同时处理 ACK 和扫描仪数据: 空载荷→ACK 通知; 含数据→构造 can_frame 并调用 `mod_can_parse_scanner()` 解析
- LoRa 网关协议枚举: `LORA_PROT_NODE(0)` / `LORA_PROT_LG210(1)` / `LORA_PROT_LG220(2)`，对应 AT+LORAPROT 的 NODE/LG210/LG220
- LoRa 工作模式枚举: `LORA_GW_MODE_FP(0)` / `LORA_GW_MODE_TRANS(1)` / `LORA_GW_MODE_NETWORK(2)`，对应 AT+WMODE 的 FP/TRANS/NET
- LoRa 初始化 (`lora_serial_init`, SYS_INIT APPLICATION level 10): 硬件复位 → 注册 async 回调 → 启动 DMA → 进入 AT 模式读取通信参数 (prot/mode/spd/ch) → 再次进入 AT 模式读取 NID/GWID → 写入 `global_params` → 恢复数据模式
