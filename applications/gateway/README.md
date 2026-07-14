# Gateway - 数据中转网关

运行在 STM32F103RCT6 上的 Zephyr RTOS 嵌入式应用，作为 mod_handler（手持控制器）与上位机之间的数据中转网关。

## 功能

- **nRF24L01P 无线接收** — 从 mod_handler 接收遥测数据和扫描仪数据
- **CAN 转发** — 双向转发 CAN 数据（与 mod_handler 相同协议）
- **UDP 透传** — 通过 W5500 以太网与上位机进行双向 UDP 透传
- **Web 配置** — HTTP 服务器提供 Web 页面配置网络参数和 nRF24 信道
- **CAN 配置** — 通过 CAN 总线配置网络参数和 nRF24 信道
- **固件升级** — 支持 HTTP POST 和 CAN 两种方式升级固件
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

## 编译

```bash
# 标准构建
west build -b nrf24_f103rct6 applications/gateway --sysbuild --build-dir build/gateway

# 烧录
west flash

# 清理重建
west build -b nrf24_f103rct6 applications/gateway --sysbuild --build-dir build/gateway --pristine
```

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

命令类型：
- `0x01` SET_CHANNEL: `[0x01][channel 1B][reserved 6B]`
- `0x02` GET_CONFIG: `[0x02][reserved 7B]`

### 网络配置帧

| 帧 ID | 方向 | 格式 |
|-------|------|------|
| 0x106 | 平台→网关 | `[cmd 1B][data...]` |
| 0x107 | 网关→平台 | `[cmd 1B][ip 4B][reserved 3B]` |

命令类型：
- `0x01` SET_IP: `[0x01][ip0][ip1][ip2][ip3][reserved 3B]`
- `0x02` SET_MASK: `[0x02][mask0][mask1][mask2][mask3][reserved 3B]`
- `0x03` SET_GW: `[0x03][gw0][gw1][gw2][gw3][reserved 3B]`
- `0x04` SET_PORT: `[0x04][port_hi][port_lo][reserved 5B]`
- `0x05` GET_CONFIG: 查询全部配置

### 固件升级帧 (与 mod_handler 协议一致)

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台→网关 | 控制命令 (本机字节序) |
| 0x102 | 网关→平台 | 响应帧 |
| 0x103 | 平台→网关 | 固件数据 |

升级流程：
1. 发送 `BOARD_START_UPDATE` + `total_size` → 收到 `FW_CODE_OFFSET(0)`
2. 发送 `FW_DATA_RX` 数据帧 (每帧 8 字节) → 每 64 字节收到进度回复
3. 发送 `BOARD_CONFIRM` → 收到 `FW_CODE_CONFIRM(0x55AA55AA)` → 自动重启

## UDP 透传

数据包格式：`[CAN ID 2B BE][payload]`

- 上行：网关 → 上位机（nRF24 接收的数据）
- 下行：上位机 → 网关（通过 nRF24 发送到 mod_handler）

## Web 配置

浏览器访问网关 IP 地址（默认 `192.168.1.100`）。

### API

| 路由 | 方法 | 功能 |
|------|------|------|
| `/` | GET | Web 配置页面 |
| `/api/config` | GET | 获取当前配置 |
| `/api/config` | POST | 设置配置 (网络+RF24) |
| `/api/firmware` | POST | 上传固件 |
| `/api/reboot` | POST | 重启设备 |

## 数据流

```
mod_handler --nRF24--> gateway --CAN--> 上位机 (CAN)
                                  --UDP--> 上位机 (Network)
上位机 --CAN--> gateway --nRF24--> mod_handler
上位机 --UDP--> gateway --nRF24--> mod_handler
```

## 资源占用

| 区域 | 已用 | 总量 | 百分比 |
|------|------|------|--------|
| FLASH | 143KB | 195KB | 73.4% |
| RAM | 36.6KB | 48KB | 74.5% |

## 目录结构

```
gateway/
  boards/nrf24_f103rct6.overlay  -- 板级覆盖 (W5500 + nRF24 + CAN)
  boards/nrf24_f103rct6.conf     -- 板级配置 (ETH_W5500)
  include/gateway.h              -- 公共定义
  src/main.c          -- 入口 + 网络初始化
  src/rf24.c          -- nRF24L01P 收发
  src/can_forward.c   -- CAN 转发 + 网络配置
  src/udp_forward.c   -- UDP 透传
  src/web_server.c    -- HTTP 服务器
  src/fw_upgrade.c    -- CAN 固件升级
  src/config.c        -- 配置管理
  src/persist.c       -- Settings 持久化
  src/web_page.html   -- Web 前端
  CMakeLists.txt
  Kconfig
  prj.conf
  VERSION
  CLAUDE.md
```
