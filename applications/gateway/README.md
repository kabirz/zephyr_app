# Gateway - 数据中转网关

运行在 STM32F103RCT6 上的 Zephyr RTOS 嵌入式应用，作为 mod_handler（手持控制器）与上位机之间的数据中转网关。通过 nRF24L01+ 无线接收手柄数据，经 W5500 以太网 UDP 转发给上位机。

## 编译

```bash
west build -b nrf24_f103rct6 applications/gateway --sysbuild
west flash
```

## 硬件

### 组件

| 组件 | 型号 | 接口 |
|------|------|------|
| MCU | STM32F103RCT6 | ARM Cortex-M3 72MHz, 256KB Flash, 48KB RAM |
| 无线 | nRF24L01+ | SPI2 (8MHz) |
| 以太网 | Wiznet W5500 | SPI3 (8MHz) |

### 引脚分配

| 功能 | 引脚 | 总线 | 说明 |
|------|------|------|------|
| nRF24 SCK/MISO/MOSI | PB13/PB14/PB15 | SPI2 | 8MHz |
| nRF24 CS / CE / IRQ | PB12 / PA9 / PC6 | SPI2 | nRF24L01P |
| W5500 SCK/MISO/MOSI | PB3/PB4/PB5 | SPI3 | 8MHz |
| W5500 CS | PA15 | SPI3 | 片选 |
| W5500 INT / RST | PD2 / PC12 | — | 中断 / 复位 |

> PB3/PB4/PA15 是 JTAG 引脚，Zephyr pinctrl 使用 SPI3 时自动释放 JTAG（保留 SWD）。

> 网关板所有模块常供电，无软件控制的电源使能 GPIO。

## 数据流

- 上行: mod_handler → nRF24 → Gateway → UDP → 上位机
- 下行: 上位机 → UDP → Gateway → nRF24 → mod_handler

## 帧协议

帧 ID 定义在 `gateway.h::enum can_ids`（复用历史 CAN 11-bit 编号，仅作逻辑标识符）：

| 帧 ID | 名称 | 方向 | 用途 |
|-------|------|------|------|
| 0x1E3 | HANDLER_STATE | 手柄→网关 | 手柄状态 (X/Y + 按键) |
| 0x263 | OVERBREAK_LASER | 网关→手柄 | 超欠挖 + 激光测距 |
| 0x363 | COORD_XY | 网关→手柄 | X/Y 坐标 |
| 0x463 | COORD_Z | 网关→手柄 | Z 坐标 |
| 0x763 | COBID_HEATBEAT | 手柄→网关 | 心跳 |

## UDP 协议

### 双端口

| 端口 | 用途 | 可配 | 默认 |
|------|------|------|------|
| **数据端口** | nRF24↔上位机数据转发 | `UDP_CMD_SET_PORT` 可改，持久化 | 9090 |
| **配置端口** | 所有配置命令 + 固件升级 | 固定 | 9200 |

双端口均设 `SO_BROADCAST`，均按子网判断单播/广播：
- 上位机不知道设备 IP 时可向 `255.255.255.255:9200` 广播配置命令。
- **数据端口**：转发目标与本机同子网 → 单播；未学习/跨子网 → 广播 `255.255.255.255:<data_port>`。
- **配置端口**：回复同子网发送方单播；跨子网 → 广播 `255.255.255.255:9200`。

### 数据帧格式
`[帧 ID 2B BE][payload]` (透传手柄/扫描仪数据，走数据端口)

### 命令帧格式
`[cmd 1B][data...]` (无魔数头，走配置端口 9200)

### 配置命令

| 命令 | 格式 | 说明 |
|------|------|------|
| 0x01 | `[0x01][ip 4B]` | 设置 IP |
| 0x02 | `[0x02][mask 4B]` | 设置子网掩码 |
| 0x03 | `[0x03][gw 4B]` | 设置网关 |
| 0x04 | `[0x04][port 2B BE]` | 设置数据端口 (配置端口 9200 固定不变) |
| 0x05 | `[0x05]` | 查询配置 (返回: `[0x05][rf24_ch 1B][rf24_addr 5B][data_port 2B BE][config_port 2B BE]`) |
| 0x06 | `[0x06]` | 查询版本 (返回: `[0x06][APP_VERSION_STRING 变长]`) |
| 0x07 | `[0x07][channel 1B]` | 设置 RF24 信道 |
| 0x08 | `[0x08][addr 5B]` | 设置 RF24 地址 |
| 0x09 | `[0x09]` | 重启设备 |

### 固件升级命令

| 命令 | 格式 | 说明 |
|------|------|------|
| 0x10 | `[0x10]` | 开始固件升级 |
| 0x11 | `[0x11][data...]` | 固件数据 (每包最大 256B) |
| 0x12 | `[0x12]` | 结束固件升级并重启 |

## Shell 命令

```
rf24 info                       查看 channel/addr/device 状态
rf24 ch [0-125]                 get/set 信道
rf24 addr <b0 b1 b2 b3 b4>      设置 5 字节地址
rf24 send <text...>             发送 DATA 帧
rf24 ping [count=5] [iv_ms=200] ping/echo 往返测试, 统计 RTT
rf24 listen [on|off]            切换监听 (打印 DATA + 自动回 ECHO)
rf24 diag                       打印 nRF24 硬件寄存器快照
```

## 资源占用

| FLASH | RAM |
|-------|-----|
| 147 KB / 194 KB (75.6%) | 45 KB / 48 KB (92.7%) |

## 目录结构

```
gateway/
  boards/
    nrf24_f103rct6.overlay  -- 板级覆盖 (nRF24 SPI2 + W5500 SPI3 + RTC)
  include/gateway.h          -- 公共定义 (帧 ID 枚举、配置参数)
  src/
    main.c                   -- 入口 + 网络初始化 (W5500 静态 IP)
    rf24.c                   -- nRF24L01P 收发
    rf24_shell.c             -- rf24 shell 测试命令
    udp.c                    -- UDP 透传 + 配置 + 固件升级
    config.c                 -- 配置管理
    persist.c                -- Settings 持久化
  CMakeLists.txt             -- 源文件列表
  Kconfig                    -- 线程栈/优先级配置
  prj.conf                   -- 应用配置 (含网络栈)
  VERSION
  CLAUDE.md
  README.md
```
