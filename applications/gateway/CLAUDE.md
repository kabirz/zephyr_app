# CLAUDE.md -- gateway

## 项目愿景

gateway 是一个运行在 STM32F103RCT6 上的 Zephyr RTOS 嵌入式应用，作为 **数据中转网关**，负责在 mod_handler（手持控制器）与上位机之间转发数据。

- **版本**: 0.1.0-dev
- **硬件平台**: nrf24_f103rct6 (与 mod_handler 共用)
- **Bootloader**: MCUBoot (swap-with-scratch 模式)
- **网络芯片**: Wiznet W5500 (SPI 以太网)
- **无线模块**: Nordic nRF24L01+ (SPI 接口，2.4GHz，中断驱动)

---

## 功能概述

1. **nRF24L01P 接收** — 从 mod_handler 接收遥测数据和扫描仪数据
2. **CAN 转发** — 将 nRF24 数据转发到 CAN 总线（与 mod_handler 相同协议），CAN 数据也转发到 nRF24
3. **UDP 透传** — 通过 W5500 以太网与上位机进行双向 UDP 透传
4. **Web 配置** — HTTP 服务器提供 Web 页面配置网络参数、nRF24 地址和信道
5. **固件升级** — HTTP POST 方式进行本地固件升级
6. **nRF24 配置** — 支持通过 CAN 或 Web 修改 nRF24 信道（地址固定，由芯片 UID 生成）

---

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

---

## 数据流

```
mod_handler --nRF24--> gateway --CAN--> host (CAN)
                                  --UDP--> host (Network)
host --CAN--> gateway --nRF24--> mod_handler
host --UDP--> gateway --nRF24--> mod_handler
```

## CAN 协议

与 mod_handler 完全一致：

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x1E3 | 手柄→网关 | 手柄状态 (X/Y BE + 按键) |
| 0x263 | 网关→手柄 | 超欠挖 + 激光测距 |
| 0x363 | 网关→手柄 | X/Y 坐标 |
| 0x463 | 网关→手柄 | Z 坐标 |
| 0x763 | 手柄→网关 | 心跳 |
| 0x104 | 平台→网关 | nRF24 配置命令 (本地处理) |
| 0x105 | 网关→平台 | nRF24 配置响应 |
| 0x101/0x103 | 平台→手柄 | 固件升级 (不转发) |

## UDP 透传格式

UDP 数据包格式: `[CAN ID 2B BE][payload]`
- 上行 (gateway→host): nRF24 接收的数据
- 下行 (host→gateway): 通过 nRF24 发送到 mod_handler

## Web 配置

### API
| 路由 | 方法 | 功能 |
|------|------|------|
| `/` | GET | Web 配置页面 |
| `/api/config` | GET | 获取当前配置 |
| `/api/config` | POST | 设置配置 |
| `/api/reboot` | POST | 重启设备 |

### 配置参数
- IP 地址、子网掩码、网关
- UDP 端口
- nRF24 信道 (0-125)
- nRF24 地址 (只读，由芯片 UID 生成)

---

## 源码结构

```
gateway/
  boards/nrf24_f103rct6.overlay  -- 板级覆盖 (W5500 + nRF24 + CAN)
  boards/nrf24_f103rct6.conf     -- 板级配置 (ETH_W5500)
  include/gateway.h              -- 公共定义
  src/
    main.c          -- 入口 + 网络初始化
    rf24.c          -- nRF24L01P 收发
    can_forward.c   -- CAN 转发
    udp_forward.c   -- UDP 透传
    web_server.c    -- HTTP 服务器 + Web 配置
    config.c        -- 配置管理
    persist.c       -- Settings 持久化
    web_page.html   -- Web 前端页面
  CMakeLists.txt
  Kconfig
  prj.conf
  VERSION
```

---

## 编译

```shell
west build -b nrf24_f103rct6 applications/gateway --sysbuild --build-dir build/gateway
west flash
```

---

## 关键设计决策

- **SPI2 共享**: nRF24L01P 和 W5500 共享 SPI2 总线，通过不同 CS 引脚区分 (nRF24=PB12, W5500=PA10)
- **地址固定**: nRF24 地址由芯片 UID 通过 CRC32 生成，不可修改
- **CAN 不转发固件升级**: PLATFORM_RX (0x101) 和 FW_DATA_RX (0x103) 帧不转发到 nRF24/UDP
- **CAN 配置本地处理**: RF24_CONFIG_CMD (0x104) 帧本地处理，不转发
- **Web 页面内嵌**: HTML 页面通过 gzip 压缩后嵌入固件，节省 Flash
- **POSIX API**: 使用 CONFIG_POSIX_API 启用标准 socket 接口
