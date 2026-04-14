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
  |     +-- 500ms 周期: OLED 刷新
  |
  +-- CAN 总线 (mod_can_process_thread, priority 11)
  |     +-- 固件升级协议 (fw_update)
  |     +-- 心跳发送 (mod_heart_thread, priority 11)
  |     +-- CAN 遥测: 心跳成功时发送 0x764 帧 (X/Y + 按键 + 电量)
  |     +-- LoRa 远程配参: 0x105/0x106 (k_work 异步, 不阻塞 CAN 线程)
  |     +-- 心跳失败 3 次 → connect_type = LORA_TYPE
  |
  +-- LoRa UART (WH-L101-L, UART async + DMA, lora_msg_process_thread, priority 12)
  |     +-- 数据模式: DMA 双缓冲透传, rx_timeout=20ms 判定帧边界
  |     +-- 遥测发送: 8 字节帧 (角度/按键/电量), 500ms 周期 (仅 CAN 断开时)
  |     +-- AT 模式: +++a 握手进入, 同步指令, 经 USR-LG210-L 网关中转
  |     +-- 网关管理: 透传/组网双模式, SPD/CH/NID 配置
  |
  +-- ADC 操纵杆 (adc_read_thread, priority 7)
  |     +-- X/Y 角度采集 → global_params.x/y_degree
  |     +-- 电源电压采集 → global_params.power_level (百分比)
  |
  +-- 电池 & 按键 (battery_monitor_thread, priority 7)
  |     +-- 充电状态检测 + 按键中断 (h_button 状态切换)
  |
  +-- OLED 显示 (SH1106 128x64, I2C, 主循环驱动)
  |     +-- Row 0: 连接类型 (CAN/LORA) + 24x16 水平电池图标 + 电量
  |     +-- Row 1-2: X/Y 角度值
  |     +-- Row 3: 按键状态
  |
  +-- 电源管理 (can_power_init, PRE_KERNEL_2)
        +-- 4 路 GPIO 电源开关 + PM notifier
```

### 关键设计决策

- **全局状态共享**: `gloval_params_t global_params` (定义在 `common.h`) 是所有模块共享的中心状态结构，包含操纵杆角度、电池状态、事件对象等。
- **心跳保活机制**: CAN 心跳线程在发送失败 3 次后将 `connect_type` 切换为 `LORA_TYPE`，直到 `TIMEOUT_EVENT` 重新触发。心跳成功时设置 `connect_type = CAN_TYPE` 并发送 CAN 遥测帧 (0x764)。
- **OTA 通过 CAN**: 固件升级完全通过 CAN 总线传输，分两阶段 -- 控制帧走 `PLATFORM_RX` (0x103)，数据帧走 `FW_DATA_RX` (0x103)。使用 MCUBoot swap-with-scratch 确保掉电安全。
- **外设独立供电**: CAN、LoRa、显示屏、5V 电源各有独立 GPIO 使能引脚，由 `power.c` 统一管理。
- **线程间通信**: CAN 和 LoRa 各自使用 `k_msgq`，心跳使用 `k_sem` + `k_event` 组合实现超时检测。
- **LoRa 双模式驱动**: `lora.c` 基于 UART async API + DMA 实现。数据模式使用 DMA 双缓冲 + `rx_timeout=20ms` 判定帧边界；AT 模式通过 `+++` → `a` → `+OK` 握手进入，支持同步指令收发。
- **LoRa 遥测协议**: 8 字节二进制帧 (0xAA + X/Y int16 + 按键 + 电量 + XOR校验)，独立线程 `lora_telem` 500ms 周期发送，仅在 `connect_type != CAN_TYPE` 时发送。帧格式定义在 `lora.h`。
- **CAN 遥测协议**: 帧 ID 0x764 (`COBID_TELEMETRY`)，6 字节 payload (X/Y int16 LE + 按键 + 电量)，由心跳线程在心跳成功时随周期发送。
- **CAN/LoRa 互斥**: 默认 `connect_type = CAN_TYPE` (在 `main_init` 中设置)。CAN 心跳成功 → 保持 CAN_TYPE + 发 CAN 遥测；心跳 3 次失败 → 切换 LORA_TYPE，LoRa 线程自动接管遥测；CAN 恢复 → 自动切回。
- **LoRa 网关管理**: 支持透传模式 (`LORA_GW_MODE_TRANS`, 仅 SPD+CH) 和组网模式 (`LORA_GW_MODE_NETWORK`, 需要 NID)。通过 `lora_gw_configure()` 一键配置并重启模块。也可通过 CAN 帧 0x105/0x106 远程设置/查询。
- **CAN 远程配参**: Host 通过 CAN 帧 `LORA_CONFIG_RX` (0x105) 发送 SET/QUERY 命令，设备通过 `LORA_CONFIG_TX` (0x106) 响应。配置操作耗时 10s+（AT 模式握手 + 模块重启），使用 `k_work` 异步执行，不阻塞 CAN 接收线程。命令/结果枚举定义在 `mod-can.h`。
- **OLED 显示**: 主循环 500ms 刷新全屏，使用 8x16 ASCII 字体 (`font_8x16.c`, 95 字符) + 24x16 水平电池图标 (5 级)。所有 display_write 通过 `display_mutex` 保护。
- **ADC 数据流**: 500ms 周期采集，角度写入 `global_params.x/y_degree`，电压通过 3.0V~4.2V 线性映射为 `power_level` 百分比。
- **连接类型**: `connect_type` 字段区分 `CAN_TYPE (1)` 和 `LORA_TYPE (2)`，默认 CAN。
- **Shell 调试**: `lora send/at/exit` 基础命令 + `lora gw config/query` 网关管理命令。

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
| 0x764 (`COBID_TELEMETRY`) | 手柄->平台 | 遥测 (X/Y 角度 + 按键 + 电量) |
| 0x105 (`LORA_CONFIG_RX`) | 平台->手柄 | LoRa 参数设置/查询命令 |
| 0x106 (`LORA_CONFIG_TX`) | 手柄->平台 | LoRa 参数配置/查询响应 |

## LoRa 遥测帧格式

8 字节二进制帧，主循环 500ms 周期发送至 LG210 网关:

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 帧头 | 0xAA |
| 1-2 | 2 | X 角度 | int16_t LE, 单位 0.1° |
| 3-4 | 2 | Y 角度 | int16_t LE, 单位 0.1° |
| 5 | 1 | 按键 | 0=松开, 1=按下 |
| 6 | 1 | 电量 | 0~100% |
| 7 | 1 | 校验和 | XOR(bytes 0~6) |

---

## 源码结构

```
include/
  common.h          -- 全局状态类型定义 (gloval_params_t, CAN_TYPE/LORA_TYPE)
  mod-can.h         -- CAN 协议定义 + 固件升级接口
  lora.h            -- LoRa 驱动接口 (透传/AT/遥测帧/网关配置)
  display.h         -- 显示模块接口
  power.h           -- 外设电源控制接口
src/
  main.c            -- 入口 + 主循环 (显示刷新)
  can.c             -- CAN 收发 + 心跳线程 + CAN 遥测 + LoRa 远程配参 (k_work)
  firmware.c        -- OTA 固件升级状态机
  lora.c            -- LoRa UART async+DMA + 遥测帧 (CAN 断开时) + 网关管理 + Shell
  font_8x16.c       -- 8x16 ASCII 字体位图 (95 字符, 1520 字节)
  adc.c             -- ADC 操纵杆角度采集 + 电量映射
  battery.c         -- 电池/按键 GPIO 监测
  display.c         -- SH1106 OLED 显示 (8x16 文本 + 水平电池图标)
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
| `lora_telem` | `lora_telem_thread` | 1024 | 10 | LoRa 遥测发送 (仅 CAN 断开时, 500ms) |
| `adc_thread_id` | `adc_read_thread` | 1024 | 7 | ADC 周期采集 (500ms) |
| `battery_monitor_tid` | `battery_monitor_thread` | 1024 | 7 | 电池状态监测 (5s) |

---

## 编码规范

- 使用 Zephyr LOG 模块 (`LOG_MODULE_REGISTER`, `LOG_INF/ERR/DBG/WRN`)
- 设备获取使用 devicetree 宏 (`DEVICE_DT_GET`, `GPIO_DT_SPEC_GET`)
- 线程使用 `K_THREAD_DEFINE` 静态定义
- 初始化使用 `SYS_INIT` 分级初始化
- GPIO 引脚全部通过 `zephyr,user` devicetree 节点定义
- 头文件保护使用 `#ifndef _MOD_XXX_H__` 或 `#ifndef __MOD_XXX_H__` 模式
- 显示刷新在主循环中执行，LoRa 遥测在独立线程中执行 (仅 CAN 断开时发送)
- 所有 display_write 调用通过 `display_mutex` 保护

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
- 修改遥测帧格式时，需同步更新 `lora.h` 中的帧格式注释、`lora.c` 中的 LoRa 打包逻辑、`mod-can.h` 和 `can.c` 中的 CAN 遥测逻辑
- CAN/LoRa 互斥通过 `global_params.connect_type` 控制: CAN 心跳线程写、LoRa 遥测线程读，无需额外锁 (atomic 级别的 byte 写入)
- CAN 远程配参 (0x105/0x106) 使用 `k_work` 异步处理: CAN 接收线程解析参数后 `k_work_submit()`，工作队列执行 `lora_gw_configure()` / `lora_gw_query()` 完成后发送响应帧
- `lora_cfg_work`、`pending_lora_cfg`、`lora_cfg_cmd` 为 `can.c` 内部 static 变量，由 CAN 接收线程写入、系统工作队列读取，单生产者单消费者无需锁
