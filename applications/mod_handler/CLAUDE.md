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
  +-- CAN 总线 (mod_can_process_thread, priority 11)
  |     +-- 固件升级协议 (fw_update)
  |     +-- 心跳发送 (mod_heart_thread, priority 11)
  |
  +-- LoRa UART (WH-L101-L, UART async + DMA, lora_msg_process_thread, priority 12)
  |     +-- 数据模式: DMA 双缓冲透传, rx_timeout=20ms 判定帧边界
  |     +-- AT 模式: +++a 握手进入, 同步指令, 经 USR-LG210-L 网关中转
  |
  +-- ADC 操纵杆 (adc_read_thread, priority 7)
  |     +-- X/Y 角度采集 + 电源电压
  |
  +-- 电池 & 按键 (battery_monitor_thread, priority 7)
  |     +-- 充电状态检测 + 按键中断
  |
  +-- OLED 显示 (SH1106 128x64, I2C)
  |     +-- 直接帧缓冲写入 (16x16 中文字模)
  |
  +-- 电源管理 (can_power_init, PRE_KERNEL_2)
        +-- 4 路 GPIO 电源开关 + PM notifier
```

### 关键设计决策

- **全局状态共享**: `gloval_params_t global_params` (定义在 `common.h`) 是所有模块共享的中心状态结构，包含操纵杆角度、电池状态、事件对象等。
- **心跳保活机制**: CAN 心跳线程在发送失败 3 次后自动休眠，直到 `TIMEOUT_EVENT` 重新触发。
- **OTA 通过 CAN**: 固件升级完全通过 CAN 总线传输，分两阶段 -- 控制帧走 `PLATFORM_RX` (0x103)，数据帧走 `FW_DATA_RX` (0x103)。使用 MCUBoot swap-with-scratch 确保掉电安全。
- **外设独立供电**: CAN、LoRa、显示屏、5V 电源各有独立 GPIO 使能引脚，由 `power.c` 统一管理。
- **线程间通信**: CAN 和 LoRa 各自使用 `k_msgq`，心跳使用 `k_sem` + `k_event` 组合实现超时检测。
- **LoRa 通信链路**: 使用有人物联网 WH-L101-L 模块 (串口透传)，通过 USR-LG210-L 网关与激光设备组网。HOSTWAKE 引脚 (PA0) 用于检测模块忙状态和通知模块进入发送模式。
- **LoRa 双模式驱动**: `lora.c` 基于 UART async API + DMA 实现。数据模式使用 DMA 双缓冲 + `rx_timeout=20ms` 判定帧边界；AT 模式通过 `+++` → `a` → `+OK` 握手进入，支持同步指令收发。公共接口定义在 `include/lora.h`。
- **Shell 调试**: LoRa 模块注册了 `lora send <data>`、`lora at <cmd>`、`lora exit` shell 命令用于调试。

---

## 构建与开发

### 构建

```shell
# 标准构建（带 MCUBoot sysbuild）
west build -b lora_f103rct6 . --sysbuild

# 带 shell 和 imgmgr snippet
west build -b lora_f103rct6 . --sysbuild -Dmod_handler_SNIPPET=imgmgr-shell
```

> 注意: README.md 中引用的 `laser_f103ret7` 是旧板型名，当前应使用 `lora_f103rct6`。

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
| LoRa HOSTWAKE | PA0 | GPIO 双向 |
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

---

## 源码结构

```
include/
  common.h          -- 全局状态类型定义 (gloval_params_t)
  mod-can.h         -- CAN 协议定义 + 固件升级接口
  lora.h            -- LoRa 驱动公共接口 (数据发送/AT 模式/HOSTWAKE)
  display.h         -- 显示模块接口
  power.h           -- 外设电源控制接口
src/
  main.c            -- 入口 + SYS_INIT 初始化
  can.c             -- CAN 收发 + 心跳线程
  firmware.c        -- OTA 固件升级状态机
  lora.c            -- LoRa UART async+DMA 驱动 (双模式: 数据/AT)
  adc.c             -- ADC 操纵杆角度采集
  battery.c         -- 电池/按键 GPIO 监测
  display.c         -- SH1106 OLED 显示
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
| `can_heart` | `mod_heart_thread` | 1024 | 11 | CAN 心跳保活 |
| `lora_msg` | `lora_msg_process_thread` | 1024 | 12 | LoRa 串口消息处理 |
| `adc_thread_id` | `adc_read_thread` | 1024 | 7 | ADC 周期采集 (3s) |
| `battery_monitor_tid` | `battery_monitor_thread` | 1024 | 7 | 电池状态监测 (5s) |

---

## 编码规范

- 使用 Zephyr LOG 模块 (`LOG_MODULE_REGISTER`, `LOG_INF/ERR/DBG/WRN`)
- 设备获取使用 devicetree 宏 (`DEVICE_DT_GET`, `GPIO_DT_SPEC_GET`)
- 线程使用 `K_THREAD_DEFINE` 静态定义
- 初始化使用 `SYS_INIT` 分级初始化
- GPIO 引脚全部通过 `zephyr,user` devicetree 节点定义
- 头文件保护使用 `#ifndef _MOD_XXX_H__` 或 `#ifndef __MOD_XXX_H__` 模式

---

## AI 使用指引

- 修改 CAN 协议时，同步更新 `mod-can.h` 中的枚举定义
- 添加新外设时，先在 `boards/lora_f103rct6.overlay` 的 `zephyr,user` 节点中定义引脚，然后在对应驱动中用 `GPIO_DT_SPEC_GET` / `ADC_DT_SPEC_GET_BY_IDX` 获取
- 电源管理相关代码目前被注释掉 (`CONFIG_PM` 未启用)，如需启用需在 `prj.conf` 中取消注释
- 显示模块多数函数体为空，属于预留接口
- `gloval_params_t` 中 `connect_type` 字段使用了位掩码 (`CAN_TYPE`, `LORA_TYPE`) 但当前未被使用
- Flash 分区: 内部 Flash 仅存放 mcuboot(64KB) + image-0(192KB)，image-1 和 scratch 在外部 SPI Flash (GD25Q80)
- `main.c` 中的 `#ifndef CONFIG_FLASH_SIZE` 保护是针对 native_sim 构建的兼容处理
