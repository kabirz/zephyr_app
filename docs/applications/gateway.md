# Gateway — 无线接收器

基于 Zephyr RTOS 的嵌入式无线接收器模块，运行在 STM32F103RCT6 上，作为手持控制器（mod_handler）与上位机之间的数据中转网关。通过 2.4G 无线 (nRF24L01+) 接收手持控制器数据，经 CAN 总线或以太网 (W5500) 转发至上位机。

**版本**: 0.1.0-dev | **许可证**: Apache-2.0

## 功能特性

- **nRF24L01P 无线接收** — 接收手持控制器的遥测数据和扫描仪数据
- **CAN 转发** — 将 nRF24 数据转发到 CAN 总线，协议与手持控制器一致
- **UDP 透传** — 通过 W5500 以太网与上位机进行双向 UDP 透传
- **CAN/UDP 模式切换** — 通过 PA2 按键手动切换，切换时记录到持久化存储
- **nRF24 可配置** — 信道 (0-125) 和地址 (5 字节) 均可通过 CAN 或 UDP 设置
- **CAN 网络配置** — 支持通过 CAN 总线设置 IP、掩码、网关、端口、模式
- **UDP 配置命令** — 通过 UDP magic header `[0xAA][0x55]` 发送配置命令
- **Web 配置** — HTTP 服务器提供 Web 页面配置网络参数和 nRF24 参数
- **OTA 升级** — 支持 CAN 和 UDP 两种固件升级方式
- **持久化存储** — 网络配置、nRF24 配置通过 Zephyr Settings 持久化

## 硬件要求

- MCU: STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM)
- 网络芯片: Wiznet W5500 (SPI 以太网)
- 无线模块: Nordic nRF24L01+ (SPI 接口, 2.4GHz)
- CAN 收发器: 250Kbps

## 硬件引脚分配

### SPI2 共享总线

| 功能 | 引脚 | 说明 |
|------|------|------|
| SPI2_SCK | PB13 | SPI 时钟 |
| SPI2_MISO | PB14 | 主入从出 |
| SPI2_MOSI | PB15 | 主出从入 |
| nRF24 CS | PB12 | 片选 (低有效) |
| W5500 CS | PA10 | 片选 (低有效) |

### nRF24L01+

| 功能 | 引脚 | 说明 |
|------|------|------|
| nRF24 CE | PA9 | 芯片使能 |
| nRF24 IRQ | PC6 | 中断 (下降沿) |
| nRF24 电源 | PC9 | 电源开关 |

### W5500 以太网

| 功能 | 引脚 | 说明 |
|------|------|------|
| W5500 INT | PA1 | 中断 (下降沿) |
| W5500 RESET | PB0 | 复位 (低有效) |

### CAN

| 功能 | 引脚 | 说明 |
|------|------|------|
| CAN_RX | PA11 | CAN1 接收 |
| CAN_TX | PA12 | CAN1 发送 |
| CAN 电源 | PC7 | 电源开关 |

## 数据流

```
手持控制器 --nRF24--> 无线接收器 --CAN--> 上位机 (CAN)
                                  --UDP--> 上位机 (Network)
上位机 --CAN--> 无线接收器 --nRF24--> 手持控制器
上位机 --UDP--> 无线接收器 --nRF24--> 手持控制器
```

## 构建

```shell
# 标准构建（含 MCUBoot sysbuild）
west build -b nrf24_f103rct6 applications/gateway --sysbuild --build-dir build/gateway

# 清理重建
west build -b nrf24_f103rct6 applications/gateway --sysbuild --build-dir build/gateway --pristine
```

## 烧录

```shell
# 烧录全部 (mcuboot + app)
west flash --build-dir build/gateway
```

## CAN 协议

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x1E3 | 手持控制器→接收器 | 手柄状态 (X/Y BE + 按键) |
| 0x263 | 接收器→手持控制器 | 超欠挖 + 激光测距 |
| 0x363 | 接收器→手持控制器 | X/Y 坐标 |
| 0x463 | 接收器→手持控制器 | Z 坐标 |
| 0x763 | 手持控制器→接收器 | 心跳 |
| 0x104 | 平台→接收器 | nRF24 配置命令 |
| 0x105 | 接收器→平台 | nRF24 配置响应 |
| 0x106 | 平台→接收器 | 网络配置命令 |
| 0x107 | 接收器→平台 | 网络配置响应 |
| 0x101/0x103 | 平台→手持控制器 | 固件升级 (本地处理) |

### nRF24 配置命令 (0x104)

| cmd | 说明 | 格式 |
|-----|------|------|
| 0x01 | 设置信道 | `[0x01][ch 1B][reserved 6B]` |
| 0x02 | 查询配置 | `[0x02][reserved 7B]` |
| 0x03 | 设置地址 | `[0x03][addr 5B][reserved 2B]` |

### 网络配置命令 (0x106)

| cmd | 说明 | 格式 |
|-----|------|------|
| 0x01 | 设置 IP | `[0x01][ip 4B][reserved 3B]` |
| 0x02 | 设置掩码 | `[0x02][mask 4B][reserved 3B]` |
| 0x03 | 设置网关 | `[0x03][gw 4B][reserved 3B]` |
| 0x04 | 设置端口 | `[0x04][port 2B BE][reserved 2B]` |
| 0x05 | 查询配置 | `[0x05][reserved 7B]` |
| 0x06 | 设置模式 | `[0x06][mode 1B]` (1=CAN, 2=UDP) |

## UDP 协议

- **数据帧**: `[CAN ID 2B BE][payload]` — 透传手持控制器数据
- **命令帧**: `[0xAA][0x55][cmd 1B][data...]` — 配置命令

| cmd | 说明 |
|-----|------|
| 0x01 | 设置 IP |
| 0x02 | 设置掩码 |
| 0x03 | 设置网关 |
| 0x04 | 设置端口 |
| 0x05 | 查询配置 |
| 0x06 | 设置模式 (1=CAN, 2=UDP) |
| 0x07 | 设置 RF24 信道 |
| 0x08 | 设置 RF24 地址 (5B) |
| 0x09 | 重启 |
| 0x10 | 开始固件升级 |
| 0x11 | 固件数据 |
| 0x12 | 结束固件升级 |

## 源码结构

```
gateway/
├── boards/
│   ├── nrf24_f103rct6.overlay   # 板级覆盖 (W5500 + nRF24 + CAN)
│   └── nrf24_f103rct6.conf      # 板级配置
├── include/
│   └── gateway.h                # 公共定义
├── src/
│   ├── main.c                   # 入口 + 网络初始化
│   ├── rf24.c                   # nRF24L01P 收发
│   ├── can_forward.c            # CAN 转发 + 配置命令处理
│   ├── udp_forward.c            # UDP 透传 + 配置命令处理
│   ├── config.c                 # 配置管理
│   └── persist.c                # Settings 持久化
├── CMakeLists.txt
├── Kconfig
└── prj.conf
```

## 关键设计决策

- **SPI2 共享** — nRF24L01P 和 W5500 共享 SPI2 总线，通过不同 CS 引脚区分 (nRF24=PB12, W5500=PA10)
- **地址可配置** — nRF24 地址默认 0x0000000000，可通过 CAN (0x03) 或 UDP (0x08) 设置
- **UDP-only 网络** — 48KB RAM 限制下不启用 TCP，节省约 20% RAM
- **CAN 不转发固件升级帧** — PLATFORM_RX (0x101) 和 FW_DATA_RX (0x103) 帧本地处理
- **Web 页面内嵌** — HTML 页面通过 gzip 压缩后嵌入固件，节省 Flash
- **SPI 频率协商** — SPI 驱动处理 nRF24 (2MHz) 和 W5500 (14MHz) 的频率切换
