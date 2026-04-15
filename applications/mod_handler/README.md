# Mod Handler — 激光测距手持控制器

基于 Zephyr RTOS 的嵌入式手持控制器模块，运行在 STM32F103RCT6 上，用于激光测距设备的远程操控。采集操纵杆角度和电池状态，通过 CAN 总线或 LoRa 无线与激光设备通信，提供 OLED 显示和 OTA 固件升级能力。

**版本**: 0.1.2-release | **许可证**: Apache-2.0

## 功能特性

- **操纵杆控制** — 双通道 ADC 采集 X/Y 轴角度，500ms 周期上报
- **双通道通信** — CAN 总线 (250Kbps) + LoRa 无线 (WH-L101-L 透传)，互斥冗余链路
- **CAN/LoRa 互斥** — CAN 优先发送遥测数据，心跳 3 次失败自动切换 LoRa；CAN 恢复后自动切回
- **CAN 手柄状态上报** — 帧 ID 0x1E3，8 字节 payload (X/Y 角度 BE + 按键反转 + 保留)，心跳成功时随心跳周期发送
- **CAN 扫描仪数据接收** — 帧 ID 0x263/0x363/0x463，接收超欠挖、激光测距、X/Y/Z 坐标数据
- **LoRa 遥测** — 8 字节二进制帧，包含角度/按键/电量，500ms 周期通过 LG210 网关发送 (仅 CAN 断开时)
- **LoRa 网关管理** — 支持透传/组网双模式配置，Shell 一键配置或 CAN 远程配置 SPD/CH/NID
- **CAN 远程配参** — Host 通过 CAN 帧 0x105/0x106 远程设置/查询 LoRa 网关参数
- **心跳保活** — CAN 心跳帧 (400ms 周期)，连续 3 次失败自动切换 LoRa
- **电池管理** — 充电状态检测、电量百分比估算
- **OLED 显示** — SH1106 128x64 I2C 屏幕，8x16 ASCII 字体 + 水平电池图标
- **OTA 升级** — 通过 CAN 总线接收固件，写入外部 SPI Flash，MCUBoot 安全切换
- **外设电源管理** — CAN / LoRa / 显示 / 5V 四路独立 GPIO 电源开关

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
  |     +-- 500ms 周期: OLED 刷新
  |
  +-- CAN 总线 (priority 11)
  |     +-- 消息收发与协议分发
  |     +-- 固件升级状态机
  |     +-- 心跳保活 (400ms) + 手柄状态上报 (0x1E3, 大端序)
  |     +-- 扫描仪数据接收 (0x263/0x363/0x463, 大端序解析)
  |     +-- LoRa 远程配参 (0x105/0x106, k_work 异步)
  |     +-- 心跳失败 3 次 → connect_type = LORA_TYPE
  |
  +-- LoRa UART (WH-L101-L, priority 12)
  |     +-- DMA 双缓冲透传
  |     +-- 遥测帧发送 (仅 CAN 断开时, 500ms)
  |     +-- AT 指令收发 (经 USR-LG210-L 网关中转)
  |     +-- Shell 调试命令 (send/at/exit/gw)
  |
  +-- ADC 采集 (priority 7)
  |     +-- 操纵杆 X/Y 角度 + 电源电压 (500ms)
  |
  +-- 电池监测 (priority 7)
  |     +-- 充电状态 GPIO 检测
  |     +-- 操纵手柄按键中断 (状态切换)
  |
  +-- 电源管理 (PRE_KERNEL_2 初始化)
        +-- 4 路 GPIO 电源开关
        +-- PM notifier 回调
```

### 线程清单

| 线程 | 栈大小 | 优先级 | 说明 |
|------|--------|--------|------|
| CAN 收发 | 2048 | 11 | CAN 消息接收与协议处理 |
| CAN 心跳 | 1024 | 11 | 400ms 周期心跳 + 遥测，3 次失败切 LoRa |
| LoRa 处理 | 1024 | 12 | UART DMA 双缓冲 + AT 指令解析 |
| LoRa 遥测 | 1024 | 10 | 500ms 周期遥测 (仅 CAN 断开时发送) |
| ADC 采集 | 1024 | 7 | 500ms 周期采集操纵杆角度 |
| 电池监测 | 1024 | 7 | 5 秒周期检测电池状态 |

### 线程间通信

- **全局状态**: `gloval_params_t` 结构体（操纵杆角度、电池状态、连接类型等）
- **事件通知**: `k_event` — 超时事件唤醒心跳线程
- **消息队列**: CAN 和 LoRa 各自使用 `k_msgq` 传递帧数据
- **信号量**: `k_sem` 配合心跳超时检测

## OLED 显示布局

SH1106 128x64 像素，使用 8x16 ASCII 字体，4 行 × 16 列字符：

```
Row 0: CAN     [电池图标] 85% CHG    ← 连接类型 + 水平电池图标 + 电量
Row 1: D:12345  OB:+123              ← 激光距离 + 超欠挖值 (来自扫描仪)
Row 2: X:+15.0  Y:-3.5              ← X/Y 轴角度 (-20° ~ +20°)
Row 3: BTN:OFF                        ← 按键状态 (ON/OFF)
```

主循环 500ms 周期刷新全屏，通过 mutex 保护并发 display_write。

## LoRa 遥测帧格式

仅当 CAN 断开时 (`connect_type = LORA_TYPE`)，由独立线程 500ms 周期发送 8 字节遥测帧至 LG210 网关：

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 帧头 | 0xAA |
| 1-2 | 2 | X 角度 | int16_t LE, 单位 0.1° (如 +15.0° = 150) |
| 3-4 | 2 | Y 角度 | int16_t LE, 单位 0.1° |
| 5 | 1 | 按键 | 0=松开, 1=按下 |
| 6 | 1 | 电量 | 0~100% |
| 7 | 1 | 校验和 | XOR(bytes 0~6) |

## LoRa Shell 命令

```
lora send <data>              -- 透传模式发送
lora at <cmd>                 -- 发送 AT 指令 (自动进入 AT 模式)
lora exit                     -- 退出 AT 模式
lora gw config [trans|net] [spd] [ch] [nid]  -- 网关参数配置
lora gw query                 -- 查询当前网关参数 (模式/SPD/CH/NID)
```

## CAN 通信协议

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台 → 手柄 | 控制命令（升级启动/确认/版本查询/重启） |
| 0x102 | 手柄 → 平台 | 响应帧 |
| 0x103 | 平台 → 手柄 | 固件数据传输 |
| 0x763 | 手柄 → 平台 | 心跳保活 (400ms) |
| 0x1E3 | 手柄 → 平台 | 手柄状态 (X/Y 角度 BE + 按键) |
| 0x263 | 平台 → 手柄 | 超欠挖 + 激光测距数据 |
| 0x363 | 平台 → 手柄 | X/Y 坐标数据 |
| 0x463 | 平台 → 手柄 | Z 坐标数据 |
| 0x105 | 平台 → 手柄 | LoRa 参数设置/查询命令 |
| 0x106 | 手柄 → 平台 | LoRa 参数配置/查询响应 |

### CAN 手柄状态帧格式 (ID 0x1E3, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | coord_x | int16_t BE, 单位 0.1° |
| 2-3 | 2 | coord_y | int16_t BE, 单位 0.1° |
| 4 | 1 | btn flags | bit0: btnHandler(反转), bit1: btnBox |
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

Host 通过 CAN 帧 0x105 设置或查询 LoRa 网关参数，设备通过 0x106 响应。配置操作由 `k_work` 异步执行（AT 模式握手 + 模块重启约 10s），不阻塞 CAN 接收线程。

**SET 命令 (0x105, data[0] = 0x01):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 命令 | 0x01 = SET |
| 1 | 1 | 模式 | 0=透传, 1=组网 |
| 2 | 1 | SPD | 速率等级 1-12 |
| 3 | 1 | CH | 信道 0-127 |
| 4-5 | 2 | NID | uint16_t LE, 组网模式有效 |

**QUERY 命令 (0x105, data[0] = 0x02):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 命令 | 0x02 = QUERY |

**响应 (0x106):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 结果 | 0x00=成功, 0x01=失败 |
| 1 | 1 | 模式 | 0=透传, 1=组网 (QUERY 有效) |
| 2 | 1 | SPD | 速率等级 (QUERY 有效) |
| 3 | 1 | CH | 信道 (QUERY 有效) |
| 4-5 | 2 | NID | uint16_t LE (QUERY 有效) |

### CAN/LoRa 互斥机制

系统默认 `connect_type = CAN_TYPE`，CAN 优先发送遥测数据：

1. CAN 心跳成功 → `connect_type = CAN_TYPE`，随心跳周期发送 CAN 遥测帧 (0x764)
2. CAN 心跳连续 3 次失败 → `connect_type = LORA_TYPE`，LoRa 遥测线程自动开始发送
3. CAN 恢复后 → 心跳成功自动切回 CAN，LoRa 遥测线程停止发送

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
| LoRa UART | PA3 / PA2 | USART2, 115200bps (WH-L101-L) |
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
| LoRa HOSTWAKE | PA0 | GPIO 输入 |
| LoRa RESET | PB3 | GPIO 输出 |

## 目录结构

```
include/
  common.h          -- 全局状态类型 (gloval_params_t)
  mod-can.h         -- CAN 协议定义 + OTA 接口
  lora.h            -- LoRa 驱动接口 (透传/AT/遥测/网关配置)
  display.h         -- 显示模块接口
  power.h           -- 外设电源控制接口
src/
  main.c            -- 入口 + 主循环 (显示刷新)
  can.c             -- CAN 收发 + 心跳线程 + CAN 遥测 + LoRa 远程配参
  firmware.c        -- OTA 固件升级状态机
  lora.c            -- LoRa UART async+DMA + 遥测帧 (CAN 断开时) + 网关管理 + Shell
  font_8x16.c       -- 8x16 ASCII 字体位图 (95 字符)
  adc.c             -- ADC 操纵杆角度采集 + 电量映射
  battery.c         -- 电池/按键 GPIO 监测
  display.c         -- SH1106 OLED 显示 (8x16 文本 + 电池图标)
  power.c           -- 外设电源管理
boards/
  lora_f103rct6.overlay  -- 板级 Devicetree 覆盖
sysbuild.conf            -- MCUBoot sysbuild 配置
sysbuild/mcuboot.conf    -- MCUBoot 编译参数
VERSION                  -- 版本号定义
```
