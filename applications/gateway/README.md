# Gateway - 数据中转网关

运行在 STM32F103RCT6 上的 Zephyr RTOS 嵌入式应用，作为 mod_handler（手持控制器）与上位机之间的数据中转网关。

## 功能

- **nRF24L01P 无线接收** — 从 mod_handler 接收遥测数据和扫描仪数据
- **CAN 转发** — 双向转发 CAN 数据（与 mod_handler 相同协议）
- **UDP 透传** — 通过 W5500 以太网与上位机进行双向 UDP 透传
- **UDP 配置** — 通过 UDP 命令配置网络参数和 nRF24 信道
- **UDP 固件升级** — 通过 UDP 传输固件数据升级
- **CAN 配置** — 通过 CAN 总线配置网络参数和 nRF24 信道
- **CAN 固件升级** — 使用 libs/can_fw_upgrade 共享库
- **模式切换** — PA2 按键切换 CAN/UDP 数据转发模式
- **配置持久化** — 网络参数和 nRF24 配置掉电保存

## 硬件

| 组件 | 型号 | 接口 |
|------|------|------|
| MCU | STM32F103RCT6 | - |
| 无线 | nRF24L01+ | SPI2 (8MHz) |
| 以太网 | W5500 | SPI2 (8MHz, 共享) |
| Flash | GD25Q80 (8MB) | SPI1 (30MHz) |

### 引脚分配

| 功能 | 引脚 | 说明 |
|------|------|------|
| SPI2_SCK/MISO/MOSI | PB13/PB14/PB15 | SPI 共享总线 |
| nRF24 CS | PB12 | nRF24 片选 |
| nRF24 CE | PA9 | nRF24 使能 |
| nRF24 IRQ | PC6 | nRF24 中断 |
| W5500 CS | PA10 | W5500 片选 |
| W5500 INT | PA1 | W5500 中断 |
| W5500 RESET | PB0 | W5500 复位 |
| CAN_RX/TX | PA11/PA12 | CAN 250Kbps |
| Link Switch | PA2 | 按键切换 CAN/UDP 模式 |

## 编译

```bash
west build -b nrf24_f103rct6 applications/gateway --sysbuild --build-dir build/gateway
west flash
```

## 数据流

**CAN 模式：**
- 上行: mod_handler → nRF24 → Gateway → CAN → 上位机
- 下行: 上位机 → CAN → Gateway → nRF24 → mod_handler

**UDP 模式：**
- 上行: mod_handler → nRF24 → Gateway → UDP → 上位机
- 下行: 上位机 → UDP → Gateway → nRF24 → mod_handler

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

### 网络配置帧

| 帧 ID | 方向 | 格式 |
|-------|------|------|
| 0x106 | 平台→网关 | `[cmd 1B][data...]` |
| 0x107 | 网关→平台 | `[cmd 1B][ip 4B][reserved 3B]` |

### 固件升级帧 (使用 libs/can_fw_upgrade)

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台→网关 | 控制命令 |
| 0x102 | 网关→平台 | 响应帧 |
| 0x103 | 平台→网关 | 固件数据 |

## UDP 协议

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
link udp       -- 切换到 UDP 模式
link status    -- 显示当前模式
```

## 资源占用

| 区域 | 已用 | 总量 | 百分比 |
|------|------|------|--------|
| FLASH | 144KB | 195KB | 73.7% |
| RAM | 41.4KB | 48KB | 84.2% |

## 目录结构

```
gateway/
  boards/nrf24_f103rct6.overlay  -- 板级覆盖 (W5500 + nRF24 + CAN)
  boards/nrf24_f103rct6.conf     -- 板级配置 (ETH_W5500)
  include/gateway.h              -- 公共定义
  src/
    main.c          -- 入口 + 网络初始化 + link switch
    rf24.c          -- nRF24L01P 收发
    can_forward.c   -- CAN 转发 + 网络配置 + 固件升级
    udp_forward.c   -- UDP 透传 + 配置 + 固件升级
    config.c        -- 配置管理
    persist.c       -- Settings 持久化
  CMakeLists.txt
  Kconfig
  prj.conf
  VERSION
  CLAUDE.md
  README.md
```
