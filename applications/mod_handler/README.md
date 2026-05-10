# Mod Handler — 激光测距手持控制器

基于 Zephyr RTOS 的嵌入式手持控制器模块，运行在 STM32F103RCT6 上，用于激光测距设备的远程操控。采集操纵杆角度和电池状态，通过 CAN 总线或 LoRa 无线与激光设备通信，提供 OLED 显示、系统休眠和 OTA 固件升级能力。

**版本**: 0.1.2-release | **许可证**: Apache-2.0

## 功能特性

- **操纵杆控制** — 双通道 ADC 采集 X/Y 轴角度，变化时即时上报 + 显示刷新
- **双通道通信** — CAN 总线 (250Kbps) + LoRa 无线 (WH-L101-L 透传)，互斥冗余链路
- **CAN/LoRa 手动切换** — 通过 link_switch 按键 (PA10) 手动切换，切换时关闭对方电源并重新初始化，启动默认 CAN
- **CAN 手柄状态上报** — 帧 ID 0x1E3，8 字节 payload (X/Y 角度 BE + 按键反转 + 保留)，心跳成功时周期发送，角度/按键变化时即时发送
- **CAN 扫描仪数据接收** — 帧 ID 0x263/0x363/0x463，接收超欠挖、激光测距、X/Y/Z 坐标，即时显示
- **LoRa 遥测** — 统一帧格式 (NID+Length+Data+CRC16)，角度/按键变化时即时发送 (仅 CAN 断开时)，半双工节流 (发送后等待上位机响应或 500ms 超时)
- **LoRa 链路检测** — 应用层心跳，周期发送遥测帧，连续 3 次 ACK 超时判定断连
- **LoRa 网关管理** — 支持 FP/TRANS/NET 三种工作模式，NODE/LG210/LG220 三种协议，Shell 一键配置或 CAN 远程配置
- **CAN 远程配参** — Host 通过 CAN 帧 0x105/0x106 远程设置/查询 LoRa 通信参数 (prot+mode+spd+ch)、节点 ID (NID)、网关 ID (GWID)，所有多字节字段使用大端序
- **LoRa 数据接收** — 网关下发的扫描仪数据与 CAN 帧格式一致，复用 `mod_can_parse_scanner` 解析
- **心跳保活** — CAN 心跳帧 (400ms 周期)，连续 3 次失败停止心跳循环
- **电池管理** — 充电状态 GPIO 检测、电源电压 ADC 采集 (5s 周期)，毫伏阈值映射电量图标
- **OLED 显示** — SH1106 128x64 I2C 屏幕，事件驱动刷新，Row 0 使用图标（信号/电池/连接类型），变化时即时更新对应行
- **系统休眠/唤醒** — 电源键触发或 10 分钟无操作自动休眠，关闭所有外设电源；唤醒时重新上电并刷新显示
- **OTA 升级** — 通过 CAN 总线接收固件，写入外部 SPI Flash，MCUBoot 安全切换
- **外设电源管理** — CAN / LoRa / 显示 / 手柄 四路独立 GPIO 电源开关，整合在 gpio.c 统一管理

## 硬件要求

- MCU: STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM)
- 外部 SPI Flash: GD25Q80 (存放 OTA 升级镜像)
- LoRa 模块: 有人物联网 WH-L101-L (串口透传, UART 115200bps)
- LoRa 网关: 有人物联网 USR-LG210-L
- OLED: SH1106 128x64 (I2C 地址 0x3C)
- CAN 收发器: 250Kbps

## 构建

```shell
# 标准构建（含 MCUBoot sysbuild）
west build -b lora_f103rct6 . --sysbuild

# 带 shell 和 imgmgr 调试工具
west build -b lora_f103rct6 . --sysbuild -Dmod_handler_SNIPPET=imgmgr-shell

# 清理重建
west build -b lora_f103rct6 . --sysbuild --pristine
```

## 烧录

```shell
west flash
```

## 系统架构

```
main() + SYS_INIT
  |
  +-- 主循环 (main thread)
  |     +-- 启动时初始全屏刷新, 事件驱动休眠/唤醒
  |     +-- 10 分钟无操作自动休眠, 关闭所有外设电源
  |
  +-- CAN 总线 (priority 11)
  |     +-- 消息收发与协议分发
  |     +-- 固件升级状态机
  |     +-- 心跳保活 (400ms) + 手柄状态上报 (0x1E3, 大端序)
  |     +-- 扫描仪数据接收 (0x263/0x363/0x463, 即时解析+显示)
  |     +-- LoRa 远程配参 (0x105/0x106, k_work 异步)
  |
  +-- LoRa UART (WH-L101-L, priority 12)
  |     +-- DMA 双缓冲透传
  |     +-- 遥测帧发送 (仅 CAN 断开时, 事件驱动)
  |     +-- 接收帧解析: 心跳 ACK + 扫描仪数据 (复用 CAN 解析)
  |     +-- AT 指令收发 (经 USR-LG210-L 网关中转)
  |     +-- Shell 调试命令 (send/at/exit/gw)
  |
  +-- LoRa 心跳 (priority 12)
  |     +-- 仅 connect_type == LORA_TYPE 时运行
  |     +-- 周期发送遥测帧, 等待网关 ACK
  |     +-- 连续 3 次超时 → lora_connected = false
  |
  +-- ADC 采集 (priority 7)
  |     +-- 操纵杆 X/Y 角度 (500ms, 变化时即时显示+发送)
  |     +-- 电源电压 (5s 周期, 变化时即时显示)
  |
  +-- GPIO 按键 + 电源管理 (priority 7, PRE_KERNEL_2 初始化)
        +-- 按键防抖: ISR → k_work_delayable, 延时读 GPIO 电平确认
        +-- 操纵手柄按键: 边沿检测, 电平变化时触发
        +-- 操纵手柄按键 (ISR → k_work → 即时显示+发送)
        +-- 电源键 (ISR → k_work → 休眠/唤醒)
        +-- link_switch (ISR → k_work → CAN/LoRa 切换 + 对方电源关闭)
        +-- 充电状态 GPIO 检测
        +-- 4 路 GPIO 电源开关 (CAN/LoRa/显示/手柄)
```

### 线程清单

| 线程 | 栈大小 | 优先级 | 说明 |
|------|--------|--------|------|
| CAN 收发 | 2048 | 11 | CAN 消息接收与协议处理 |
| CAN 心跳 | 1024 | 11 | 400ms 周期心跳 + 手柄状态上报，3 次失败停止 |
| LoRa 处理 | 1024 | 12 | UART DMA 双缓冲 + AT 指令解析 + 接收数据分发 |
| LoRa 心跳 | 1024 | 12 | 仅 LoRa 模式运行，周期遥测 + ACK 检测 |
| ADC 采集 | 1024 | 7 | 500ms 周期采集操纵杆角度，变化时即时显示+发送 |

### 线程间通信

- **全局状态**: `gloval_params_t` 结构体（操纵杆角度、电池状态、扫描仪数据、连接类型等）
- **事件通知**: `k_event` — WAKE_EVENT(休眠唤醒), CAN_EVENT/LORA_EVENT(连接切换)
- **消息队列**: CAN 和 LoRa 各自使用 `k_msgq` 传递帧数据
- **信号量**: `k_sem` 配合 LoRa ACK 检测
- **工作队列**: `k_work` / `k_work_delayable` — 按键防抖、显示+发送、LoRa 配参异步执行
- **互斥锁**: `lora_mode_mutex` (AT 模式), `lora_tx_mutex` (LoRA TX), `display_mutex` (显示)

## OLED 显示布局

SH1106 128x64 像素，使用 8x16 ASCII 字体 + 独立位图图标，4 行 × 16 列字符：

```
Row 0: [CAN/LORA图标][信号图标]  NID     [电池图标]  ← 连接类型 + 信号 + NID + 电池
Row 1: OB:+12345      Dis:12345             ← 超欠挖 + 激光距离 (来自扫描仪)
Row 2: X:+15.0      Y:-3.5                 ← X/Y 轴角度 (-20° ~ +20°)
Row 3: BTN:OFF                              ← 按键状态 (ON/OFF)
```

Row 0 图标说明:
- **连接类型图标** (8x16): CAN 或 LORA 标签图标
- **信号图标** (16x16): LoRa RSSI 信号强度，0-4 级柱状图
- **电池图标** (24x16): 毫伏阈值映射 4 级电量图标，充电时显示闪电标记
- **NID**: 8 位十六进制节点 ID

启动时全屏刷新一次，之后各模块事件驱动更新对应行。所有 `display_write` 通过 `display_mutex` 保护，`display_str_pad` 用空格填充行尾清除旧数据残留。

## CAN 通信协议

帧 ID 定义在 `mod-can.h`:

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台 → 手柄 | 控制命令（升级启动/确认/版本查询/重启） |
| 0x102 | 手柄 → 平台 | 响应帧 |
| 0x103 | 平台 → 手柄 | 固件数据传输 |
| 0x763 | 手柄 → 平台 | 心跳保活 (400ms) |
| 0x1E3 | 手柄 → 平台 | 手柄状态 (X/Y 角度 BE + 按键反转 + 保留) |
| 0x263 | 平台 → 手柄 | 超欠挖 + 激光测距数据 |
| 0x363 | 平台 → 手柄 | X/Y 坐标数据 |
| 0x463 | 平台 → 平台 | Z 坐标数据 |
| 0x105 | 平台 → 手柄 | LoRa 参数设置/查询命令 |
| 0x106 | 手柄 → 平台 | LoRa 参数配置/查询响应 |

### CAN 手柄状态帧格式 (ID 0x1E3, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | coord_x | int16_t BE, 单位 0.1° |
| 2-3 | 2 | coord_y | int16_t BE, 单位 0.1° |
| 4 | 1 | btn flags | bit0: btnHandler(反转), 按下=0, 松开=1 |
| 5-7 | 3 | reserved | 固定 0xFF |

### CAN 超欠挖+激光测距帧格式 (ID 0x263, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | flags | bit0: overbreak_valid, bit1: laser_valid |
| 1 | 1 | reserved | — |
| 2-3 | 2 | overbreak_value | int16_t BE, 超欠挖值 |
| 4-7 | 4 | laser_value | uint32_t BE, 激光测距值 |

### CAN X/Y 坐标帧格式 (ID 0x363, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-3 | 4 | coordX | int32_t BE, X 坐标 |
| 4-7 | 4 | coordY | int32_t BE, Y 坐标 |

### CAN Z 坐标帧格式 (ID 0x463, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-3 | 4 | coordZ | int32_t BE, Z 坐标 |
| 4 | 1 | flags | bit0: coordz_valid (同时设置 X/Y/Z 有效) |
| 5-7 | 3 | reserved | — |

### CAN 控制 LoRa 参数

Host 通过 CAN 帧 0x105 设置或查询 LoRa 参数，设备通过 0x106 响应。所有多字节字段使用大端序 (BE)。

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

**QUERY 通信参数 (0x105, data[0] = 0x02):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 命令 | 0x02 = QUERY |

**SET/QUERY 响应 (0x106, DLC 8):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 结果 | 0x00=成功, 0x01=失败 |
| 1 | 1 | prot[7:4] + mode[3:0] | 同 SET 格式 |
| 2 | 1 | SPD | 速率等级 |
| 3 | 1 | CH | 信道 |

**QUERY_NID (0x105, data[0] = 0x03) / SET_NID (0x105, data[0] = 0x04):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 命令 | 0x03 = QUERY_NID, 0x04 = SET_NID |
| 4-7 | 4 | NID | uint32_t BE (仅 SET_NID 携带) |

**NID 响应 (0x106, DLC 8):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 结果 | 0x00=成功, 0x01=失败 |
| 4-7 | 4 | NID | uint32_t BE |

**QUERY_GWID (0x105, data[0] = 0x05) / SET_GWID (0x105, data[0] = 0x06):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 命令 | 0x05 = QUERY_GWID, 0x06 = SET_GWID |
| 4-7 | 4 | GWID | uint32_t BE (仅 SET_GWID 携带) |

**GWID 响应 (0x106, DLC 8):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 结果 | 0x00=成功, 0x01=失败 |
| 4-7 | 4 | GWID | uint32_t BE |

### CAN/LoRa 连接切换

系统默认 `connect_type = CAN_TYPE`，通过 link_switch 按键 (PA10) 手动切换：

1. 按下 link_switch → ISR → k_work → CAN ↔ LoRa 切换
2. 切换时关闭对方外设电源 (lora_deinit/can_power_enable)
3. 切换时即时刷新 OLED Row 0
4. CAN 模式：角度/按键变化时通过 CAN 发送，心跳线程周期发送手柄状态帧
5. LoRa 模式：角度/按键变化时通过 LoRa 发送遥测帧，LoRa 心跳线程维护链路检测

## LoRa 统一帧格式

数据模式完整帧 (含帧头帧尾):

```
[0xAA][0x55][NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE][\r\n]
  帧头      ───────── 统一帧 ──────────                   帧尾
```

CRC 覆盖: NID + Length + Data (不含帧头帧尾)。AT 模式使用纯文本，不使用帧头帧尾。

| 帧类型 | 方向 | Data 内容 | 总长度 |
|--------|------|-----------|--------|
| 遥测/心跳 | 手柄→网关 | `[0x01][X 2B][Y 2B][btn][0xFF×3]` (9B) | 17 字节 |
| 心跳 ACK | 网关→手柄 | 空 (Length=0) | 8 字节 |
| RSSI 请求 | 手柄→网关 | `[0x03]` (1B) | 9 字节 |
| RSSI 响应 | 网关→手柄 | `[0x03][rssi 1B]` (2B) | 10 字节 |
| 扫描仪数据 | 网关→手柄 | `[Type][CAN ID 2B BE][CAN data NB]` | 变长 |
| 测试数据 | 双向 | `[0x02][payload NB]` | 变长 |

### LoRa 遥测数据 (Data 9 字节, 类型 0x01 + 与 CAN 0x1E3 一致的 8 字节)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | coord_x | int16_t BE, 单位 0.1° |
| 2-3 | 2 | coord_y | int16_t BE, 单位 0.1° |
| 4 | 1 | btn flags | bit0: btnHandler(反转), 按下=0, 松开=1 |
| 5-7 | 3 | reserved | 固定 0xFF |

### LoRa 扫描仪数据 (网关→手柄)

Data 字段 = `[Type 1B] + [CAN frame ID 2B BE] + [CAN data bytes]`，与 CAN 帧数据一致，复用 `mod_can_parse_scanner` 解析。

### LoRa 链路检测

- 心跳线程仅 `connect_type == LORA_TYPE` 时运行
- 周期发送遥测帧，等待网关 ACK (空载荷帧)
- 连续 3 次超时 → `lora_connected = false`
- 收到任何合法帧 (NID 匹配 + CRC 正确) → `lora_connected = true`
- `lora_is_connected()` 供外部查询链路状态

### LoRa 网关参数

| 参数 | AT 指令 | 说明 |
|------|---------|------|
| 协议 | AT+LORAPROT | NODE (点对点), LG210 (默认), LG220 |
| 工作模式 | AT+WMODE | FP (点对点), TRANS (透传, 默认), NET (组网) |
| 速率 | AT+SPD | 1-12, 默认 10 |
| 信道 | AT+CH | 0-127, 默认 72 (470MHz) |
| 网关 ID | AT+GWID | uint32_t, 组网模式有效 |
| 节点 ID | AT+NID | uint32_t |

## Shell 命令

```
link can                                  -- 切换到 CAN 模式
link lora                                 -- 切换到 LoRa 模式
lora send <data>                           -- 透传模式发送
lora at <cmd>                              -- 发送 AT 指令 (自动进入 AT 模式, 支持直接输入 AT+ 前缀)
lora exit                                  -- 退出 AT 模式
lora gw config [prot] [mode] [spd] [ch]   -- 配置通信参数
  prot: node, lg210 (default), lg220
  mode: trans (default), fp, net
lora gw query                              -- 查询当前通信参数
lora nid [hex_value]                       -- 查询/设置节点 ID
lora gwid [hex_value]                      -- 查询/设置网关 ID
```

## OTA 升级流程

1. 平台通过 CAN 帧 0x101 发送升级启动命令
2. 手柄初始化外部 SPI Flash 写入上下文
3. 平台通过 CAN 帧 0x103 分片传输固件数据
4. 传输完成后验证镜像，确认写入成功
5. MCUBoot 以 swap-with-scratch 模式完成安全切换

## 引脚分配

| 功能 | 引脚 | 说明 |
|------|------|------|
| CAN TX/RX | PA12 / PA11 | CAN1, 250Kbps |
| LoRa UART | PA3 / PA2 | USART2, 115200bps (WH-L101-L, DMA1 CH6/CH7) |
| 调试串口 | PB7 / PB6 | USART1 (remap), 115200bps |
| OLED I2C | PB11 / PB10 | I2C2, SH1106 @ 0x3C |
| SPI Flash | PA5/PA6/PA7/PA4 | SPI1, GD25Q80 |
| CAN 电源 | PC7 | GPIO 输出 |
| 显示电源 | PC8 | GPIO 输出 |
| LoRa 电源 | PC9 | GPIO 输出 |
| 5V 使能 | PA8 | GPIO 输出 |
| ADC-X | PC4 (ADC1_CH14) | 操纵杆 X 轴 |
| ADC-Y | PC5 (ADC1_CH15) | 操纵杆 Y 轴 |
| ADC-VCC | PB1 (ADC1_CH9) | 电源电压采样 |
| 操纵手柄按键 | PB0 | GPIO 输入 |
| 充电满 | PB12 | GPIO 输入上拉，低有效 |
| 充电中 | PB13 | GPIO 输入上拉，低有效 |
| 电源键 | PB14 | GPIO 输入上拉 |
| Link Switch | PA10 | GPIO 输入上拉 (CAN/LoRa 手动切换) |
| LoRa HOSTWAKE | PA0 | GPIO 输入 |
| LoRa RESET | PB3 | GPIO 输出 |

## 目录结构

```
include/
  common.h          -- 全局状态类型 (gloval_params_t, scanner_data_t, 事件位定义)
  mod-can.h         -- CAN 协议定义 + OTA 接口 + LoRa 配参命令/结果枚举
  lora.h            -- LoRa 驱动接口 (透传/AT/遥测/网关配置/NID-GWID管理/链路检测/统一帧常量)
  display.h         -- 显示模块接口
  mod-gpio.h        -- GPIO 电源控制 + 按键 + 电池状态 + CAN/LoRa 切换
  battery_icons.h   -- 电池图标位图 (24x16, 4 级电量 + 充电动画)
  label_icons.h     -- 连接类型标签图标位图 (8x16, CAN/LORA)
  signal_icons.h    -- LoRa 信号强度图标位图 (16x16, 0-4 级)
src/
  main.c            -- 入口 + 主循环 (休眠/唤醒事件驱动)
  can.c             -- CAN 收发 + 心跳线程 + 手柄状态上报 + 扫描仪数据解析 + LoRa 远程配参
  firmware.c        -- OTA 固件升级状态机
  lora.c            -- LoRa UART async+DMA + 遥测帧 + 接收数据分发 + 心跳线程 + 网关管理 + Shell
  font_8x16.c       -- 8x16 ASCII 字体位图 (95 字符)
  adc.c             -- ADC 操纵杆角度采集 + 电源电压采样
  gpio.c            -- GPIO 按键防抖 + 电源管理 + CAN/LoRa 切换 + 休眠/唤醒 + Shell
  display.c         -- SH1106 OLED 显示 (8x16 文本 + 位图图标, display_str_pad 防残留)
boards/
  lora_f103rct6.overlay  -- 板级 Devicetree 覆盖
sysbuild.conf            -- MCUBoot sysbuild 配置
sysbuild/mcuboot.conf    -- MCUBoot 编译参数
VERSION                  -- 版本号定义
```
