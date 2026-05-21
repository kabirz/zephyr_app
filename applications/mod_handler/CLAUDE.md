# CLAUDE.md -- mod_handler

## 项目愿景

mod_handler 是一个运行在 STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM) 上的 Zephyr RTOS 嵌入式应用。它作为激光测距系统的**手持控制器模块**，负责采集操纵杆角度、电池状态，并通过 CAN 总线或 LoRa 无线与激光设备通信，同时提供 OLED 显示、系统休眠和 OTA 固件升级能力。

- **版本**: 0.1.2-release
- **硬件平台**: lora_f103rct6 (自定义板)
- **Bootloader**: MCUBoot (swap-with-scratch 模式)
- **LoRa 模块**: 有人物联网 WH-L101-L (串口透传, UART AT 指令)
- **LoRa 网关**: 有人物联网 USR-LG210-L
- **许可证**: Apache-2.0

---

## 架构总览

系统采用 Zephyr 多线程架构，通过 `gloval_params_t` 全局结构体在模块间共享状态，使用 `k_event` 进行跨线程事件通知。外围设备电源由 GPIO 独立控制，整合在 `gpio.c` 统一管理，支持系统休眠/唤醒。

```
main() + SYS_INIT
  |
  +-- 主循环 (main thread)
  |     +-- 启动时初始全屏刷新, 事件驱动休眠/唤醒
  |     +-- 10 分钟无操作自动休眠, 关闭所有外设电源
  |
  +-- CAN 总线 (mod_can_process_thread, priority 11)
  |     +-- 固件升级协议 (fw_update)
  |     +-- 心跳发送 (mod_can_thread, priority 11)
  |     +-- 手柄状态上报: 心跳成功时发送 0x1E3 帧 (X/Y BE + 按键反转 + 0xFF)
  |     +-- 扫描仪数据接收: 0x263/0x363/0x463 (即时解析+显示)
  |     +-- LoRa 远程配参: 0x105/0x106 (专用工作队列异步, 不阻塞 CAN 线程)
  |     +-- 心跳失败 3 次 → 停止心跳循环
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
  |     +-- 周期发送遥测帧, 等待网关 ACK
  |     +-- 连续 3 次失败 → lora_connected = false
  |
  +-- ADC 操纵杆 (adc_read_thread, priority 7)
  |     +-- X/Y 角度采集 → 500ms, 变化时即时显示 + CAN/LoRa 发送
  |     +-- 电源电压采集 → 5s 周期, 变化时即时显示
  |
  +-- GPIO 按键 + 电源管理 (power_init, PRE_KERNEL_2; gpio_init, APPLICATION)
        +-- 按键防抖: ISR → k_work_delayable, 延时读 GPIO 电平确认
        +-- 操纵手柄按键 (ISR → k_work_delayable → 即时显示+发送)
        +-- 电源键 (ISR → k_work_delayable → 休眠/唤醒)
        +-- link_switch (ISR → k_work → CAN/LoRa 切换 + 关闭对方电源)
        +-- 充电状态 GPIO 检测 (read_battery_status)
        +-- 4 路 GPIO 电源开关 (CAN/LoRa/显示/手柄)
```

### 关键设计决策

- **全局状态共享**: `gloval_params_t global_params` (定义在 `display.c`) 是所有模块共享的中心状态结构，包含操纵杆角度、电池状态、扫描仪数据 (`scanner_data_t`)、事件对象等。
- **事件系统**: `k_event` 定义 4 个事件位 — `WAKE_EVENT BIT(0)` (休眠唤醒), `CAN_RX_EVENT BIT(1)` (CAN 数据接收), `CAN_EVENT BIT(2)` (CAN 激活), `LORA_EVENT BIT(3)` (LoRa 激活)。
- **事件驱动显示**: 启动时全屏刷新一次，之后各模块在数据变化时即时更新对应 OLED 行。`display_str_pad` 渲染字符串后用空格填充行尾，防止旧数据残留。显示函数接口拆分为细粒度参数（如 `mod_display_handler_xy(int x, int y)` 而非 `gloval_params_t*`），便于跨模块直接调用。
- **心跳保活机制**: CAN 心跳线程周期发送心跳帧，失败 3 次后停止循环。心跳成功时发送手柄状态帧 (0x1E3, 大端序)。
- **连接类型切换**: 通过 link_switch 按键 (PA10) 手动切换 CAN/LoRa。ISR 中提交 k_work，工作队列中调用 `canlora_switch()` 关闭对方电源 + 重新初始化 + 即时刷新 OLED Row 0。启动默认 CAN_TYPE。
- **系统休眠/唤醒**: 电源键触发或 10 分钟无操作自动休眠，关闭所有外设电源 (can_power_enable/dis_power_enable/handler_power_enable + lora_deinit)。唤醒时重新上电、lora_init (若 LoRa 模式)、等待 200ms 后 reinit 显示并全屏刷新。
- **外设独立供电**: CAN、LoRa、显示屏、手柄电源各有独立 GPIO 使能引脚，整合在 `gpio.c` 统一管理 (`power_init` PRE_KERNEL_2 初始化)。
- **按键防抖**: 所有按键 (电源键、操纵手柄键) 使用 `k_work_delayable` + `k_work_reschedule` 实现延时防抖。ISR 中提交延时 work (10ms)，work handler 中读取 GPIO 电平确认后执行业务逻辑。操纵手柄按键使用边沿检测 (`gpio_pin_get_dt() != global_params.h_button`)，电源键使用电平确认 (`gpio_pin_get_dt() == 0`)。
- **OTA 通过 CAN**: 固件升级完全通过 CAN 总线传输，分两阶段 -- 控制帧走 `PLATFORM_RX` (0x101)，数据帧走 `FW_DATA_RX` (0x103)。使用 MCUBoot swap-with-scratch 确保掉电安全。
- **线程间通信**: CAN 和 LoRa 各自使用 `k_msgq`，LoRa ACK 使用 `k_sem`，按键/配参使用 `k_work`/`k_work_delayable`，显示使用 `display_mutex`。
- **LoRa 双模式驱动**: `lora.c` 基于 UART async API + DMA 实现。数据模式使用 DMA 双缓冲 + `rx_timeout=20ms` 判定帧边界；AT 模式通过 `+++` → `a` → `+OK` 握手进入，支持同步指令收发。`lora_mode_mutex` 保护 AT 模式切换，`lora_tx_mutex` 保护多线程 TX 竞争。
- **LoRa 统一帧格式**: 数据模式完整帧 = `[0xAA][0x55][NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE][\r\n]`。帧头 0xAA 0x55，帧尾 \r\n，CRC 覆盖 NID+Length+Data。`lora_data_send()` 自动组帧（含帧头帧尾），接收线程通过 0xAA 0x55 + \r\n 边界检测提取统一帧后调用 `parse_lora_frame()` 解析。AT 模式使用纯文本，不使用帧头帧尾。启动消息 "LoRa Start!\r\n" 无帧头，特殊处理。常量定义在 `lora.h`: `LORA_FRAME_OVERHEAD=8`, `LORA_FRAME_HEADER_SIZE=6`。
- **LoRa 数据类型**: Data 首字节为类型标识 — `LORA_DATA_HANDLER(0x01)` 遥测、`LORA_DATA_TEST(0x02)` 测试、`LORA_DATA_RSSI(0x03)` RSSI 请求/响应。空载荷 (Length=0) 为心跳 ACK。RSSI 响应 Data = `[0x03][rssi 1B]`。
- **LoRa 遥测协议**: Data 9 字节 (`[0x01][X 2B BE][Y 2B BE][btn][0xFF 3B]`)，事件驱动发送。角度或按键变化时，若 `connect_type != CAN_TYPE` 则通过 LoRa 发送。发送后通过 `lora_data_sem` 阻塞等待上位机响应或 500ms 超时，实现半双工节流。
- **LoRa 接收数据 (合并帧)**: 网关下发的扫描仪数据使用合并帧格式，Data 字段 = `[0x01][flags 1B][overbreak 2B BE][laser 4B BE][coord_x 4B BE][coord_y 4B BE][coord_z 4B BE]` = 20 字节。flags: bit0=overbreak_valid, bit1=laser_valid, bit2=coord_z_valid, bit3=coord_xy_valid。`handle_scanner_data()` 按 payload_len >= 20 识别合并帧，直接解析写入 `scanner_data_t`；旧格式 (< 20 字节, `[CAN frame ID 2B BE][CAN data NB]`) 仍兼容，复用 `mod_can_parse_scanner()` 解析。收到数据后 `k_sem_give(&lora_data_sem)` 释放遥测发送许可。
- **LoRa 链路检测**: 应用层心跳，仅 `connect_type == LORA_TYPE` 时由 `lora_heartbeat_thread` 维护。周期发遥测帧，ACK 超时，连续 3 次失败 → `lora_connected = false`。收到合法帧 (NID 匹配 + CRC 正确) → `lora_connected = true`。`lora_is_connected()` 供外部查询。ACK 通过 `lora_ack_sem` (k_sem) 在接收线程和心跳线程间通知。
- **LoRa init/deinit 生命周期**: `lora_init()` = 上电 + GPIO 硬件复位 + 2s 等待启动 + 停止旧 DMA + 重新启动 DMA 双缓冲接收。`lora_deinit()` = 停止 DMA + 断电。CAN/LoRa 切换、系统休眠/唤醒时调用，避免 UART 悬空导致 DMA 状态损坏。
- **CAN 手柄状态协议**: 帧 ID 0x1E3 (`HANDLER_STATE`)，8 字节 payload (X/Y int16 BE + 按键反转 + 0xFF 保留)，由心跳线程在心跳成功时随周期发送，同时在角度/按键变化时由 ADC/按键模块即时发送。所有多字节数据使用大端序 (网络字节序)，按键 btnHandler 反转 (按下=0, 松开=1)。
- **CAN 扫描仪数据接收**: 帧 ID 0x263 (`OVERBREAK_LASER`), 0x363 (`COORD_XY`), 0x463 (`COORD_Z`)，接收扫描仪下发的超欠挖+激光测距、X/Y/Z 坐标数据。解析后存入 `global_params.scanner` 并即时触发显示刷新。Z 坐标有效标志同时设置 X/Y/Z 三者的有效性。
- **CAN/LoRa 切换**: 通过 link_switch 按键手动切换 `connect_type`。`canlora_switch()` 在 k_work 中执行：切换时 clear 对方事件 (CAN_EVENT|CAN_RX_EVENT 或 LORA_EVENT)、调用 lora_deinit()/can_power_enable(false) 关闭对方电源、重新初始化活跃方电源、set 事件 (CAN_EVENT|CAN_RX_EVENT 或 LORA_EVENT)、刷新 OLED Row 0。
- **LoRa 网关管理**: 支持三种通信协议 (`LORA_PROT_NODE`/`LORA_PROT_LG210`/`LORA_PROT_LG220`, AT+LORAPROT) 和三种工作模式 (`LORA_GW_MODE_FP` 点对点 / `LORA_GW_MODE_TRANS` 透传 / `LORA_GW_MODE_NETWORK` 组网, AT+WMODE)。透传/点对点模式仅需 SPD+CH，组网模式额外需要 GWID。默认 prot=LG210, mode=TRANS。GWID 和 NID 通过独立的 AT 指令 (AT+GWID/AT+NID) 设置，与通信参数 (prot/mode/spd/ch) 分离管理。
- **CAN 远程配参**: Host 通过 CAN 帧 `LORA_CONFIG_RX` (0x105) 发送 SET/QUERY/QUERY_NID/SET_NID/QUERY_GWID/SET_GWID 命令，设备通过 `LORA_CONFIG_TX` (0x106) 响应。0x105 SET 格式: data[0]=0x01(SET), data[1]=prot[7:4]|mode[3:0], data[2]=SPD, data[3]=CH (不再包含 GWID)。0x105 QUERY: data[0]=0x02。0x105 QUERY_NID: data[0]=0x03。0x105 SET_NID: data[0]=0x04, data[4-7]=NID(BE)。0x105 QUERY_GWID: data[0]=0x05。0x105 SET_GWID: data[0]=0x06, data[4-7]=GWID(BE)。所有多字节字段使用大端序。配置操作耗时 10s+（AT 模式握手 + 模块重启），使用 `k_work` 异步执行，不阻塞 CAN 接收线程。命令/结果枚举定义在 `mod-can.h`。
- **OLED 显示**: 事件驱动刷新。启动时 `mod_display_all` 全屏刷新，之后各模块在数据变化时调用对应行刷新函数。Row 0 使用独立位图图标：连接类型图标 (`label_icons.h`, 8x16, CAN/LORA)、信号强度图标 (`signal_icons.h`, 16x16, 0-4级)、NID 文本、电池图标 (`battery_icons.h`, 24x16, 4级电量+充电动画)。`display_str_pad` 渲染后用空格填充行尾清除旧数据残留。所有 `display_write` 通过 `display_mutex` 保护。显示函数接口为细粒度参数（如 `mod_display_handler_xy(int x, int y)`），不依赖完整 `gloval_params_t` 指针。
- **ADC 数据流**: 500ms 周期采集 X/Y 角度，变化时即时显示 + CAN/LoRa 发送。电压每 10 个周期 (5s) 采集一次，变化时即时显示。
- **按键事件流**: GPIO ISR 检测按键下降沿 → `k_work_reschedule(&work, K_MSEC(10))` → work handler 中读取 GPIO 电平确认 → 执行业务逻辑 (显示+发送/休眠/切换)。ISR 中不做显示/发送操作。
- **连接类型**: `connect_type` 字段区分 `CAN_TYPE (1)` 和 `LORA_TYPE (2)`，默认 CAN。通过 link_switch (PA10) 手动切换，切换时即时刷新 OLED Row 0。
- **Shell 调试**: `link can/lora` (tab 补全切换 CAN/LoRa) + `lora send/at/exit` 基础命令 (at 支持直接输入 `AT+` 前缀) + `lora rssi` 发送 RSSI 请求 + `lora test on/off` 手动进入/退出测试模式 (shell 设置时跳过 RSSI 轮询, `test_mode_from_shell` 区分 shell 与上位机设置的测试模式) + `lora gw config [prot] [mode] [spd] [ch]` 通信参数配置 + `lora gw query` 通信参数查询 + `lora nid [hex]` 查询/设置节点 ID + `lora gwid [hex]` 查询/设置网关 ID。

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
| Link Switch | PA10 | GPIO 输入上拉 (CAN/LoRa 手动切换) |
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
[NID 4B BE][Length 2B BE][Data NB][CRC16 2B BE]
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

### LoRa 扫描仪数据 (网关→手柄, 合并帧)

合并帧 payload 20 字节，一次传输所有扫描仪数据:

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | type | 0x01 (LORA_DATA_HANDLER) |
| 1 | 1 | flags | bit0=overbreak_valid, bit1=laser_valid, bit2=coord_z_valid, bit3=coord_xy_valid |
| 2 | 2 | overbreak | int16_t BE |
| 4 | 4 | laser | uint32_t BE |
| 8 | 4 | coord_x | int32_t BE |
| 12 | 4 | coord_y | int32_t BE |
| 16 | 4 | coord_z | int32_t BE |

旧格式 (`[CAN frame ID 2B BE][CAN data bytes]`, payload < 20 字节) 仍兼容，复用 `mod_can_parse_scanner()` 解析。

### LoRa 链路检测

- 心跳线程仅 `connect_type == LORA_TYPE` 时运行
- 周期发送遥测帧，等待网关 ACK (空载荷帧)
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

### Shell 命令

```
link can                                  -- 切换到 CAN 模式 (支持 tab 补全)
link lora                                 -- 切换到 LoRa 模式 (支持 tab 补全)
lora send <data>                           -- 透传模式发送
lora at <cmd>                              -- 发送 AT 指令 (自动进入 AT 模式, 支持直接输入 AT+ 前缀)
lora exit                                  -- 退出 AT 模式
lora rssi                                  -- 发送 RSSI 信号强度请求
lora test on                               -- 进入测试模式 (shell 手动, 跳过 RSSI 轮询)
lora test off                              -- 退出测试模式
lora test stats                            -- 显示测试统计
lora test reset                            -- 重置测试统计
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
  common.h          -- 全局状态类型定义 (gloval_params_t, scanner_data_t, 事件位定义, CAN_TYPE/LORA_TYPE)
  mod-can.h         -- CAN 协议定义 + 固件升级接口 + LoRa 配参命令/结果枚举
  lora.h            -- LoRa 驱动接口 (透传/AT/遥测帧/网关配置/NID-GWID管理/链路检测/统一帧常量/init/deinit)
  display.h         -- 显示模块接口
  mod-gpio.h        -- GPIO 电源控制 + 按键 + 电池状态 + CAN/LoRa 切换
  battery_icons.h   -- 电池图标位图 (24x16, 4级电量 + 充电动画, battery_levels/battery_charging/battery_full)
  label_icons.h     -- 连接类型标签图标位图 (8x16, label_can/label_lora)
  signal_icons.h    -- LoRa 信号强度图标位图 (16x16, signal_levels 0-4级)
src/
  main.c            -- 入口 + 主循环 (休眠/唤醒事件驱动, 10 分钟无操作自动休眠)
  can.c             -- CAN 收发 + 心跳线程 + 手柄状态上报 (0x1E3 BE) + 扫描仪数据解析+显示 + LoRa 远程配参 (k_work)
  firmware.c        -- OTA 固件升级状态机
  lora.c            -- LoRa UART async+DMA + 统一帧收发 + 遥测 (事件驱动) + 接收数据分发 + 心跳线程 + 网关管理 + Shell
  font_8x16.c       -- 8x16 ASCII 字体位图 (95 字符, 1520 字节)
  adc.c             -- ADC 操纵杆角度采集 + 电源电压采样 (变化时即时显示+发送)
  gpio.c            -- GPIO 按键防抖 + 电源管理 + CAN/LoRa 切换 + 休眠/唤醒 + Shell (link can/lora)
  display.c         -- SH1106 OLED 显示 (8x16 文本 + 位图图标, display_str_pad 防残留, global_params 定义)
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
| `lora_heart` | `lora_heartbeat_thread` | 1024 | 12 | LoRa 心跳 (仅 LORA_TYPE 时运行, 周期遥测 + ACK 检测) |
| `adc_thread_id` | `adc_read_thread` | 1024 | 7 | ADC 周期采集 (500ms 角度, 5s 电压, 变化时即时发送) |
| `lora_cfg_workq` | `k_work_queue` | 2048 | 8 | CAN LoRa 远程配参专用工作队列 (避免与 STM32 UART 驱动 RX 超时 k_work_delayable 死锁) |

注意: 原电池监测线程已合并到 gpio.c，按键事件通过 ISR + k_work_delayable 处理，不再需要独立线程。

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
- ISR 中通过 `k_work_reschedule` 提交延时任务，不在中断上下文做显示/发送操作
- 按键防抖使用 `k_work_delayable`，ISR 中 reschedule，work handler 中读 GPIO 电平确认

---

## AI 使用指引

- 修改 CAN 协议时，同步更新 `mod-can.h` 中的枚举定义
- 添加新外设时，先在 `boards/lora_f103rct6.overlay` 的 `zephyr,user` 节点中定义引脚，然后在对应驱动中用 `GPIO_DT_SPEC_GET` / `ADC_DT_SPEC_GET_BY_IDX` 获取
- `gloval_params_t` 中 `connect_type` 使用 `CAN_TYPE (1)` / `LORA_TYPE (2)` 区分连接类型，通过 link_switch (PA10) 手动切换，不再由心跳自动切换
- `global_params` 定义在 `display.c`，声明在 `common.h`
- 事件位定义: `WAKE_EVENT BIT(0)`, `CAN_RX_EVENT BIT(1)`, `CAN_EVENT BIT(2)`, `LORA_EVENT BIT(3)`，不再使用 `TIMEOUT_EVENT`
- Flash 分区: 内部 Flash 仅存放 mcuboot(64KB) + image-0(192KB)，image-1 和 scratch 在外部 SPI Flash (GD25Q80)
- `main.c` 中的 `#ifndef CONFIG_FLASH_SIZE` 保护是针对 native_sim 构建的兼容处理
- 字体/图标位图取模方式: Column-major, page-interleaved, LSB=top pixel (与 SH1106 原生格式一致)。图标数据按 page0 cols[0..N-1] 然后 page1 cols[0..N-1] 排列
- 图标位图定义在独立头文件: `battery_icons.h` (24x16, 电池)、`label_icons.h` (8x16, 连接类型标签)、`signal_icons.h` (16x16, 信号强度)
- Row 0 显示布局: 连接类型图标 (8x16) + 信号图标 (16x16) + NID 文本 + 电池图标 (24x16)
- 显示接口: `mod_display_lora(rssi)`, `mod_display_can()`, `mod_display_lora_nid(nid)`, `mod_display_battery(power_mv, status)`, `mod_display_scanner(s)`, `mod_display_handler_xy(x, y)`, `mod_display_handler_button(btn)`
- `mod_display_init` 中通过 `handler_get_btn()` 读取初始按键状态写入 `global_params.h_button`
- `mod_display_battery` 参数从 `(uint8_t power_level)` 改为 `(uint32_t power_mv, battery_status_t status)`，电源电压使用 ADC 毫伏值，毫伏阈值映射电量图标 (≥3850mV=4级, ≥3750mV=3级, ≥3550mV=2级, ≥3400mV=1级, <3400mV=0级)
- NID 显示函数: `mod_display_lora_nid(uint32_t nid)`, Row 0 显示 8 位十六进制 NID
- LoRa AT 十六进制参数解析使用通用函数 `parse_at_hex`，NID/GWID 查询均使用此函数
- LoRa AT 指令格式参考 `doc/` 目录下的 WH-L101-L AT 指令集和 LG210 协议说明书
- 修改遥测帧格式时，需同步更新 `lora.h` 中的帧格式注释和常量、`lora.c` 中的统一帧打包/解析逻辑、`mod-can.h` 和 `can.c` 中的手柄状态逻辑
- CAN 帧使用大端序 (网络字节序): 发送用 `sys_put_be16`/`sys_put_be32`，接收用 `sys_get_be16`/`sys_get_be32`
- CAN/LoRa 切换通过 `global_params.connect_type` 控制: link_switch 按键 ISR→k_work→`canlora_switch()` 关闭对方电源 + 重新初始化
- 扫描仪数据通过 `global_params.scanner` (scanner_data_t) 存储，CAN 接收线程或 LoRa 接收线程写入并即时触发显示刷新
- LoRa 半双工节流: `lora_send_telemetry()` 发送成功后 `k_sem_take(&lora_data_sem, K_MSEC(500))` 阻塞等待，接收线程在 `handle_scanner_data()` 中 `k_sem_give(&lora_data_sem)` 释放。超时 500ms 后自动继续发送
- LoRa 帧接收使用 `ring_buf` 环形缓冲区 (`lora_rx_rb`) 替代线性 `asm_buf` + `memmove`，`lora_msg_process_thread` 通过 `ring_buf_peek` + `ring_buf_get` 实现 0xAA 0x55 帧头检测和帧提取
- Shell 测试模式: `lora test on/off` 通过 `test_mode_from_shell` 标志区分 shell 手动设置和上位机 RSSI 响应设置的测试模式。RSSI 轮询线程 (`lora_rssi_thread`) 仅在 `test_mode_from_shell` 时跳过，上位机设置的不跳过
- CAN 远程配参 (0x105/0x106) 使用专用工作队列 (`lora_cfg_workq`, 优先级 8, 栈 2048) 异步处理: CAN 接收线程解析参数后 `k_work_submit_to_queue()`，工作队列执行 `lora_gw_configure()` / `lora_gw_query()` 完成后发送响应帧。不使用系统工作队列是因为 STM32 UART async 驱动的 RX 超时通过 `k_work_delayable` 提交到系统工作队列，若配参也占用系统工作队列会死锁
- CAN 0x105 SET 帧格式: data[1] 高 4 位 = prot, 低 4 位 = mode, data[2]=SPD, data[3]=CH (不含 GWID)。NID/GWID 独立命令: QUERY_NID(0x03)/SET_NID(0x04), QUERY_GWID(0x05)/SET_GWID(0x06)。NID/GWID 数据使用 BE (sys_put_be32/sys_get_be32)。SET_NID/SET_GWID 需 AT 模式写模块并重启, 通过 k_work 异步执行
- `lora_cfg_work`、`pending_lora_cfg`、`pending_nid`、`pending_gwid`、`lora_cfg_cmd` 为 `can.c` 内部 static 变量，由 CAN 接收线程写入、专用工作队列 `lora_cfg_workq` 读取，单生产者单消费者无需锁
- `btn_display_work`、`sleep_work`、`linksw_work` 为 `gpio.c` 内部 static 变量，ISR 中 `k_work_reschedule`/`k_work_submit`，工作队列线程执行
- 按键防抖: `btn_display_work` 和 `sleep_work` 使用 `k_work_delayable`，ISR 中 reschedule，work handler 中 `gpio_pin_get_dt()` 确认。操纵手柄按键使用边沿检测 (`!= global_params.h_button`)，电源键使用低电平确认 (`== 0`)
- 电源控制函数定义在 `gpio.c`，声明在 `mod-gpio.h`: `can_power_enable()`, `lora_power_enable()`, `dis_power_enable()`, `handler_power_enable()`, `handler_get_btn()`
- 电池状态读取: `read_battery_status()` 定义在 `gpio.c`，声明在 `mod-gpio.h`，返回 `battery_status_t` 枚举
- LoRa TX 线程安全: `lora_tx_mutex` 保护 `lora_data_send()`，防止 ADC 线程、按键 k_work、心跳线程并发 TX 竞争
- LoRa 链路状态: `lora_connected` 为 `atomic_t`，接收线程置 true，心跳线程连续超时置 false，`lora_is_connected()` 供外部无锁读取
- LoRa ACK 通知: `lora_ack_sem` (k_sem, max=1)，接收线程解析到空载荷帧 (Length=0) 时 `k_sem_give()`，心跳线程 `k_sem_take()` 等待 ACK
- LoRa 接收线程同时处理 ACK 和扫描仪数据: 空载荷→ACK 通知; 含数据→按 Data 首字节类型分发 (HANDLER/TEST/RSSI)，扫描仪数据构造 can_frame 并调用 `mod_can_parse_scanner()` 解析
- LoRa 网关协议枚举: `LORA_PROT_NODE(0)` / `LORA_PROT_LG210(1)` / `LORA_PROT_LG220(2)`，对应 AT+LORAPROT 的 NODE/LG210/LG220
- LoRa 工作模式枚举: `LORA_GW_MODE_FP(0)` / `LORA_GW_MODE_TRANS(1)` / `LORA_GW_MODE_NETWORK(2)`，对应 AT+WMODE 的 FP/TRANS/NET
- LoRa 初始化 (`lora_serial_init`, SYS_INIT APPLICATION level 10): GPIO 配置 → HOSTWAKE 配置 → 注册 UART async 回调 → `lora_init()` (上电+复位+2s等待+重启DMA) → AT 模式读取通信参数 (prot/mode/spd/ch) → AT 模式读取 NID/GWID → 写入 `global_params` → 刷新 NID 显示 → 恢复数据模式
- LoRa deinit: `lora_deinit()` = `lora_rx_disable_sync()` (停止 DMA) + `lora_power_enable(false)` (断电)，用于 CAN/LoRa 切换和系统休眠
- 系统休眠: `system_sleep()` 关闭所有外设电源 (can_power_enable/lora_power_enable/dis_power_enable/handler_power_enable)，设置 `sleeping = true`，clear WAKE_EVENT
- 系统唤醒: `system_wake()` 重新上电所有外设，LoRa 模式下调用 `lora_init()`，等待 200ms 后 `mod_display_reinit()` + `mod_display_all()`，设置 `sleeping = false`
- `power.c` 和 `power.h` 已删除，电源管理代码整合到 `gpio.c`，接口声明在 `mod-gpio.h`
- `battery.c` 已删除，电池/按键/电源管理整合到 `gpio.c`
