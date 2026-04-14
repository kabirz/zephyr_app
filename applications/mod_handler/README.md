# Mod Handler — 激光测距手持控制器

基于 Zephyr RTOS 的嵌入式手持控制器模块，运行在 STM32F103RCT6 上，用于激光测距设备的远程操控。采集操纵杆角度和电池状态，通过 CAN 总线或 LoRa 无线与激光设备通信，提供 OLED 显示和 OTA 固件升级能力。

**版本**: 0.1.2-release | **许可证**: Apache-2.0

## 功能特性

- **操纵杆控制** — 双通道 ADC 采集 X/Y 轴角度，3 秒周期上报
- **双通道通信** — CAN 总线 (250Kbps) + LoRa 无线 (WH-L101-L 透传)，冗余链路
- **心跳保活** — CAN 心跳帧 (400ms 周期)，连续 3 次失败自动休眠
- **电池管理** — 充电状态检测、电量百分比估算
- **OLED 显示** — SH1106 128x64 I2C 屏幕，16x16 中文字模
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
  +-- CAN 总线 (priority 11)
  |     +-- 消息收发与协议分发
  |     +-- 固件升级状态机
  |     +-- 心跳保活 (400ms)
  |
  +-- LoRa UART (WH-L101-L, priority 12)
  |     +-- AT 指令收发 (经 USR-LG210-L 网关中转)
  |     +-- Shell 调试命令 (lora send <data>)
  |
  +-- ADC 采集 (priority 7)
  |     +-- 操纵杆 X/Y 角度
  |     +-- 电源电压采样
  |
  +-- 电池监测 (priority 7)
  |     +-- 充电状态 GPIO 检测
  |     +-- 操纵手柄按键中断
  |
  +-- OLED 显示 (SH1106, I2C)
  |
  +-- 电源管理 (PRE_KERNEL_2 初始化)
        +-- 4 路 GPIO 电源开关
        +-- PM notifier 回调
```

### 线程清单

| 线程 | 栈大小 | 优先级 | 说明 |
|------|--------|--------|------|
| CAN 收发 | 2048 | 11 | CAN 消息接收与协议处理 |
| CAN 心跳 | 1024 | 11 | 400ms 周期心跳，3 次失败休眠 |
| LoRa 处理 | 1024 | 12 | UART AT 指令解析 |
| ADC 采集 | 1024 | 7 | 3 秒周期采集操纵杆角度 |
| 电池监测 | 1024 | 7 | 5 秒周期检测电池状态 |

### 线程间通信

- **全局状态**: `gloval_params_t` 结构体（操纵杆角度、电池状态、连接类型等）
- **事件通知**: `k_event` — 超时事件唤醒心跳线程
- **消息队列**: CAN 和 LoRa 各自使用 `k_msgq` 传递帧数据
- **信号量**: `k_sem` 配合心跳超时检测

## CAN 通信协议

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台 → 手柄 | 控制命令（升级启动/确认/版本查询/重启） |
| 0x102 | 手柄 → 平台 | 响应帧 |
| 0x103 | 平台 → 手柄 | 固件数据传输 |
| 0x763 | 手柄 → 平台 | 心跳保活 (400ms) |

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
| LoRa HOSTWAKE | PA0 | GPIO 双向 |
| LoRa RESET | PB3 | GPIO 输出 |

## 目录结构

```
include/
  common.h          -- 全局状态类型 (gloval_params_t)
  mod-can.h         -- CAN 协议定义 + OTA 接口
  display.h         -- 显示模块接口
  power.h           -- 外设电源控制接口
src/
  main.c            -- 入口 + SYS_INIT 初始化
  can.c             -- CAN 收发 + 心跳线程
  firmware.c        -- OTA 固件升级状态机
  lora.c            -- LoRa UART 通信 + Shell 命令
  adc.c             -- ADC 操纵杆角度采集
  battery.c         -- 电池/按键 GPIO 监测
  display.c         -- SH1106 OLED 显示
  power.c           -- 外设电源管理
boards/
  lora_f103rct6.overlay  -- 板级 Devicetree 覆盖
sysbuild.conf            -- MCUBoot sysbuild 配置
sysbuild/mcuboot.conf    -- MCUBoot 编译参数
VERSION                  -- 版本号定义
```
