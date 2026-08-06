# Data Collect (数据采集节点)

基于 Zephyr RTOS 的数据采集 DAQ 节点。采集数字量输入（DI）、数字量输出（DO）、模拟量输入（AI），
并通过 **Modbus RTU（RS485）/ Modbus TCP（Raw ADU）** 对外提供数据采集与参数读写；参数可通过
**UDP 配置端口** 与 **CAN 配置帧** 两种通道以二进制命令帧下发（与 gateway 上位机协议同构）；
固件升级支持 **UDP** 与 **CAN** 双通道。历史数据按采样周期写入 littlefs（挂载于 SPI Flash）。

---

## 目录

- [硬件信息](#一硬件信息)
- [功能描述](#二功能描述)
- [构建与烧录](#三构建与烧录)
- [协议描述](#四协议描述)
  - [4.1 Modbus 寄存器映射](#41-modbus-寄存器映射)
  - [4.2 UDP 配置命令协议](#42-udp-配置命令协议)
  - [4.3 CAN 业务帧协议](#43-can-业务帧协议)
  - [4.4 固件升级通道](#44-固件升级通道)
- [关键实现文件](#五关键实现文件)

---

## 一、硬件信息

### 1.1 目标平台

| 项目 | 值 |
|------|----|
| SoC | STM32F407VET6（Cortex-M4F，168MHz，1MB Flash / 192KB+128KB RAM） |
| 板级目标 | `stm32f407/stm32f407xx/daq`（应用内分支 `stm32f407_daq`） |
| 备选目标 | `apollo_h743ii`（STM32H743，FDCAN） |
| Bootloader | MCUboot（sysbuild，swap-scratch 模式） |

### 1.2 外设与引脚分配（见 `boards/stm32f407_daq.overlay`）

| 外设 | 说明 | 引脚 |
|------|------|------|
| 数字量输入 DI | 16 路，GPIO 上拉输入，按使能位掩码采样 | `d0~d15`: PB15、PB14、PB13、PB12、PD0、PD11、PB6、PB7（依 overlay `di-gpios`） |
| 数字量输出 DO | 8 路 输出 + 8 路 状态 LED | `do-gpios`（PD7-PD14）、`doled-gpios`（PE8-PE15） |
| 模拟量输入 AI | 4 路 12bit ADC（AIN10-13） | PC0/PC1/PC2/PC3（`adc1` channel A-D，内部基准） |
| Modbus RTU | `usart2`（PA2/PA3，115200，半双工，RE 使能 PA1 低有效） | `modbus0`（compatible `zephyr,modbus-serial`） |
| 以太网 | STM32 内置 MAC + 最外层 PHY（可选 SPI W5500）：`zephyr,eth` | — |
| CAN | `can1`（PA11/PA12） | `zephyr,canbus = &can1` |
| SPI NOR Flash | W25Q128（SPI） | 存数据区 / 镜像槽 / 备用槽 |
| RTC | 片上 RTC | `zephyr,rtc` |
| 状态 LED | `heart_status`（编入 shell main） | 心跳闪亮 |
| 掉电存配置 | settings（FCB + NVS） | `cfg_partition` |

### 1.3 Flash 分区（MCUboot 多镜像布局）

| 分区 | 作用 | 首地址 / 长度 |
|------|------|--------------|
| 0（`mcuboot`） | MCUboot bootloader | 0x00000000, 0x20000 |
| 1（`image-0` / `slot0`） | 主应用（slot 0） | 0x00020000, 0x60000 |
| SPI NOR `data`（`storage`） | littlefs 数据区（历史） | 0x00000000, 0xd00000 |
| SPI NOR `image-1`（`slot1`） | 次应用槽（OTA） | 0x00d00000, 0x60000 |
| SPI NOR `image-scratch` | 交换暂存区 | 0x00d80000, 0x60000 |

---

## 二、功能描述

1. **数据采集**
   - DI：按 `HOLDING_DI_SI_IDX` 采样间隔读取 16 路 DI，实时反映到输入寄存器 `INPUT_DI_IDX`（bit0~15）。
   - AI：按 `HOLDING_AI_SI_IDX` 采样间隔读取 4 路 ADC，转换毫伏（或 mA）写入 `INPUT_AI0~3`。
   - DO：写 `HOLDING_DO_IDX` 立即输出 8 路 DO 与联动 LED。
2. **Modbus 服务**
   - Modbus RTU 服务器（RS485，PD 半双工，从机地址/波特率可配）。
   - Modbus TCP 服务器（Raw ADU 透传，TCP 端口可配，默认 502，支持多客户端、超时/下电自动断连）。
3. **参数配置通道**：UDP 配置端口（默认 8600，`udp_fw_upgrade` 库自管）与 CAN 配置帧（0x1A0/0x1A1），复用 `enum udp_cmd` 命令集，采用 gateway 风格二进制帧，支持网络、Modbus、采样、CAN、心跳、校时、设备信息等参数设置与读取，修改即写 `settings_save()` 幂等。
4. **历史数据落盘**：DI/AI 采样按 `HOLDING_HIS_SAVE_IDX` 使能后，按类型封包写入 little 文件 `data_<timestamp>.raw`，单文件 1MB，最多 10 个，旧文件循环覆盖。
5. **网络管理**：支持静态 IP 与 DHCP；静态模式下掩码固定 /24、网关为 `a.b.c.1`。网络就绪后启动服务。
6. **心跳**：看门狗式吊销——超过 `HOLDING_HEART_TIMEOUT_MS`   内无业务心跳则自动清零 DO（用于安全联动）。
7. **固件升级**：支持 UDP（配置端口）与 CAN（0x101-0x105）双通道在线升级，依赖 MCUboot + DFU（`stream_flash`）。
8. **校时**：`SET_TIME` / RTC。断电用 RTC 保存时间并同步系统 `CLOCK_REALTIME`。

---

## 三、构建与烧录

环境（Zephyr v4.2、Zephyr SDK、west 1.3）：

```sh
source ~/venv/rt/bin/activate
cd applications/data_collect
west build -b stm32f407/stm32f407xx/daq --sysbuild
```

- 应用与 MCUboot 均由 sysbuild 统一签名（key: `boards/rsa_mcuboot_2048_${BOARD}.pem`）。
- 产出：`build/data_collect/zephyr/zephyr.signed.hex(.bin)`，`build/mcuboot/zephyr/zephyr.hex(.bin)`。
- 支持其他板时，在 `data_collect/boards/` 放置对应板 key：`rsa_mcuboot_2048_<boardname>.pem`，无需改配置文件。

---

## 四、协议描述

### 4.1 Modbus 寄存器映射

#### 线圈（Coils，`MODBUS_COLS_REGISTER_NUMBERS`=8）
| 线圈 | 含义 |
|------|------|
| COIL 0 | DO bit0 |
| … | DO bit1..7（与 `HOLDING_DO_IDX` 对应） |

#### 输入寄存器（`MODBUS_INPUT_REGISTER_NUMBERS`=6）
| 地址 | 寄存器 | 含义 |
|------|--------|------|
| 0 | `INPUT_VER_IDX` | 固件版本（`版本号`= major<<8 \| minor） |
| 1-4 | `INPUT_AI0..3` | AI0-3 采样值（mV 或电流） |
| 5 | `INPUT_DI_IDX` | DI bit0~15 状态（每 bit 对应一路） |

#### 保持寄存器（`MODBUS_HOLDING_REGISTER_NUMBERS`=23）
| 地址 | 寄存器 | 含义 | 默认值 |
|------|--------|------|--------|
| 0  | `HOLDING_DO_IDX` | DO bit0~7（写即输出） | |
| 1  | `HOLDING_DI_EN_IDX` | DI 使能 bit 掩码 | 0xffff |
| 2  | `HOLDING_AI_EN_IDX` | AI 使能 bit 掩码 | 0xf |
| 3  | `HOLDING_DI_SI_IDX` | DI 采样间隔(ms) | 200 |
| 4  | `HOLDING_AI_SI_IDX` | AI 采样间隔(ms) | 200 |
| 5  | `HOLDING_HIS_SAVE_IDX` | 历史使能（非 0 写盘） | 0 |
| 6  | `HOLDING_CAN_ID_IDX` | CAN 节点地址(信息性) | 0x111 |
| 7  | `HOLDING_CAN_BPS_IDX` | CAN 波特率(信息性) | 10 |
| 8  | `HOLDING_RS485_BPS_IDX` | RS485 波特率 | 9600 |
| 9  | `HOLDING_SLAVE_ID_IDX` | Modbus RTU 从机号 | 1 |
| 10-13 | `HOLDING_IP_ADDR_1..4` | 静态 IP（aa.bb.cc.dd） | 192.168.12.101 |
| 14 | `HOLDING_TIMESTAMPH_IDX` | 时间戳高 16 位 | |
| 15 | `HOLDING_TIMESTAMPL_IDX` | 时间戳低 16 位 | |
| 16 | `HOLDING_CFG_SAVE_IDX` | 写 1 保存配置 | |
| 17 | `HOLDING_REBOOT_IDX` | 写 1 重启 | |
| 18 | `HOLDING_HEART_EN_IDX` | 心跳使能 | |
| 19 | `HOLDING_HEART_TIMEOUT_IDX` | 心跳超时(ms, 下限 500) | 2000 |
| 20 | `HOLDING_HEART_IDX` | 心跳计数（未用-持续清零） | |
| 21 | `HOLDING_TCP_PORT_IDX` | Modbus TCP 端口（默认 502） | 502 |
| 22 | `HOLDING_NET_MODE_IDX` | 网络模式（0 静态 / 1 DHCP） | 0 |

> 网络生效时机：`net_mode` 与 `rs485_bps`/`slave_id`/`ip` 需重启；`tcp_port`、`history`、`di_si`、`ai_si`、`do` 即时生效。

### 4.2 UDP 配置命令协议

- **入口**：UDP 配置端口默认 **8600**（`CONFIG_UDP_FW_CONFIG_PORT`），由 `udp_fw_upgrade` 库自管 RX 线程。
- **帧格式**：`[cmd 1B][data...]`，大端（BE）。`0x01-0x05` 由库内部固件升级引擎处理，不到达业务回调。
- 业务命令（复用 `udp_cmd`）：

| Cmd | 名称 | 请求 (Req) | 响应 (Resp) |
|-----|------|------------|-------------|
| `0x12` | SET_NET | `[ip4][tcp_port2 BE]=6B` | 回显 6B |
| `0x13` | GET_NET | 无 | `[ip4 (live)][tcp_port2 BE]=6B` |
| `0x16` | SET_NET_MODE | `[mode 1B]`(0 静态/1 DHCP) | 回显 1B（重启生效） |
| `0x17` | GET_NET_MODE | 无 | `[mode 1B]` |
| `0x18` | SET_MODBUS | `[slave_id 2B BE][rs485_bps 4B BE]=6B` | 回显 6B |
| `0x19` | GET_MODBUS | 无 | `[slave_id 2B][rs485_bps 4B]=6B` |
| `0x1A` | SET_SAMPLE | `[di_si 2B][ai_si 2B][his_en 1B]=5B` | 回显 5B |
| `0x1B` | GET_SAMPLE | 无 | `[di_si 2B][ai_si 2B][his_en 1B]=5B` |
| `0x1C` | SET_CAN | `[can_id 2B][can_bps 2B]=4B` | 回显 4B |
| `0x1D` | GET_CAN | 无 | `[can_id 2B][can_bps 2B]=4B` |
| `0x1E` | SET_HEART | `[heart_en 1B][timeout 2B BE]=3B` | 回显 3B |
| `0x1F` | GET_HEART | 无 | `[heart_en 1B][timeout 2B]=3B` |
| `0x20` | SET_TIME | `[timestamp 4B BE]` | 回显设置时间 4B（立即生效，同步 RTC） |
| `0x21` | GET_INFO | 无 | `[version 4B BE][uptime_s 4B BE]=8B` |

> 字节序均为 **Big-Endian**。所有 SET_* 先写 holding 寄存器再 `settings_save()`，大部分参数重启生效；响应由库 `udp_fw_reply()` 发出（同子网单播 / 跨子网广播路由）。

### 4.3 CAN 业务帧协议

CAN 由 `can_fw_upgrade` 库自管控制器与 RX 线程，固件升级帧为 `0x101-0x105`（含 keyhash 与版本字符串）。

| CAN ID | 方向 | 帧内容 |
|--------|------|--------|
| `0x763` | DAQ → Host | **周期心跳**（每 1s）：`[version 2B BE][di 2B BE][do 1B][rsv 3B]=8B` |
| `0x1A0` | Host → DAQ | **配置命令**：`[sub 1B][payload ≤7B]`，`sub` 复用 `enum udp_cmd`（0x12-0x21），语义与 UDP 完全一致 |
| `0x1A1` | DAQ → Host | **配置响应**：`[sub 1B][seq 1B][payload ≤6B]`（payload>6B 时按 `seq=0,1,..` 分帧，否则单帧 `seq=0`） |

- 命令响应负载实时构建（`dc_build_config_payload()`），UDP/CAN 共用，保证两通道参数语义与门值一致。
- CAN 心跳帧：版本（版本号 major<<8\|minor，BE）、DI 当前值、DO 低 8 位。

### 4.4 固件升级通道

| 通道 | 端口 / ID | 引擎 | 流程 |
|------|-----------|------|------|
| UDP | UDP 8600 | `udp_fw_upgrade`（待接收 `FW_START 0x1` / `FW_DATA 0x2` / `FW_END 0x3` / `GET_VERSION 0x4` / `REBOOT 0x5`） | 接收分段→`IMG_MANAGER` 校验后写入→MCUboot 重启升级 |
| CAN | `0x101`-`0x105` | `can_fw_upgrade` | 分帧接收→` >0`（同上）写第二镜像→握手 ` 版本 / keyhash` → reboot 交接 |

依赖：`CONFIG_IMG_MANAGER`、`CONFIG_STREAM_FLASH`、`CONFIG_FLASH_MAP`、`CONFIG_CRC`、`CONFIG_REBOOT`、MCUboot（sysbuild）。升级过程中配置端口/ CAN RX 线程仍可用（配置 socket 与业务命令由库自管）。

---

## 五、关键实现文件

| 文件 | 职责 |
|------|------|
| `src/main.c` | 主循环：状态 LED 心跳、堆栈溢出/冷重启自动化 |
| `src/modbus/init.{c,h}` | holding/input 寄存器定义、默认值、心跳吊销线程、ID/初始化 |
| `src/modbus/function.c` | Modbus 寄存器回调 + Zephyr settings handler（R/W/导出 `modbus/*` 键） |
| `src/modbus/tcp.c` | Modbus TCP Raw ADU 服务器（端口可配，客户端管理） |
| `src/modbus/rtu.c` | Modbus RTU 服务器（RS485，从机号/波特率可配） |
| `src/modbus/adc.c` | AI 采样线程 |
| `src/modbus/dio.c` | DI 采样线程 + DO 输出 + 输出 LED |
| `src/modbus/history.c` | 历史落盘 littlefs（`data_*.raw`，1MB×10 轮转） |
| `src/modbus/udp.c` | UDP 配置命令执行器 `dc_build_config_payload()`（UDP/CAN 共用） |
| `src/can.c` | CAN 心跳帧 + 配置命令解析/分帧响应 |
| `src/net.c` | 静态 IP / DHCP 初始化、链路/地址事件、就绪门控 |
| `src/time.c` | RTC ↔ 系统时钟同步、校时 |
| `src/fs.c` | RAM 磁盘/FAT 挂载（可选，`CONFIG_FAT_FILESYSTEM_ELM`） |
| `include/data_collect.h` | 协议契约：UDP 命令集、CAN 帧 ID、公开接口声明 |