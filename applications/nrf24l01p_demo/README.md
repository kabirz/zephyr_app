# nRF24L01P Demo — 2.4GHz 射频收发示例

基于 Zephyr RTOS 的 Nordic nRF24L01+ 射频收发器示例应用，演示自研 `nrf24l01p` 驱动的收发用法。运行在 STM32F407 (daq) 板上，通过 SPI3 与 nRF24L01P 模组通信，支持 TX（发射）与 RX（接收）两种角色，可用作双向数据链路的最小验证。

## 功能特性

- **TX 角色** — 每秒发送一帧 4 字节递增计数载荷（`[seq, 0xA5, 0x5A, ~seq]`），LOG 打印发送结果
- **RX 角色** — 进入 PRX 接收模式，循环接收（5s 超时），收到即 LOG 打印长度与内容
- **同步阻塞 API** — `nrf24_send`/`nrf24_recv` 带超时，内部封装 CE 触发、IRQ 等待、重传与 FIFO 清理
- **中断驱动** — IRQ 引脚下降沿中断 + 信号量唤醒，不占用 CPU 轮询
- **自检** — 启动时回读 `SETUP_AW` 寄存器验证 SPI 链路连通性

## 硬件要求

- MCU: STM32F407 (daq 板，Cortex-M4)
- 射频模组: nRF24L01+（亿佰特 E01 或兼容模组）
- 两块板 + 两片模组用于收发对传验证

> stm32f407 daq 板的 spi1 已接 W25Q128 NOR flash、spi2 已接 W5500 以太网，故本示例使用 spi3。

### 接线（默认 overlay，可按实际修改）

| nRF24L01P | STM32F407 | 说明 |
|---|---|---|
| SCK  | PC10 | SPI3 SCK (AF6) |
| MISO | PC11 | SPI3 MISO (AF6) |
| MOSI | PC12 | SPI3 MOSI (AF6) |
| CSN  | PC13 | SPI 片选（低有效） |
| CE   | PB0  | 模式/发送触发（高有效） |
| IRQ  | PB1  | 中断输出（低有效，下降沿） |
| VCC  | 3.3V | 电源（**勿接 5V**） |
| GND  | GND  | 地 |

## 构建

```shell
# TX 角色（默认）
west build -b stm32f407/stm32f407xx/daq . --pristine

# RX 角色
west build -b stm32f407/stm32f407xx/daq . --pristine -- -DCONFIG_NRF24L01P_DEMO_ROLE_RX=y
```

> 也可在 `prj.conf` 中显式设置 `CONFIG_NRF24L01P_DEMO_ROLE_RX=y` 选择 RX 角色。

## 烧录与验证

```shell
west flash
```

### 对传验证步骤

1. 两块板按上表接线，**两板 RF 参数须一致**（信道/地址/速率；默认 channel=76、addr=`E7 E7 E7 E7 E7`、1Mbps）。
2. 板 A 烧录 RX：串口应每 5s 打印 `RX timeout`（未收到数据时）。
3. 板 B 烧录 TX：每秒打印 `TX seq=N ok`。
4. RX 端应每秒收到 `RX len=4: [seq a5 5a ..]`，seq 递增。
5. 启动时两板均打印 `self-check SETUP_AW=0x03`（5 字节地址编码，验证 SPI 链路）。

> 若 `SETUP_AW` 读回 `0x00` 或 `0xFF`，多为 SPI 接线/CS 极性/电源问题，需排查硬件。

## 配置（Devicetree）

RF 参数在 `boards/stm32f407_daq.overlay` 的 `&nrf24` 节点配置，常用项：

| 属性 | 默认 | 说明 |
|---|---|---|
| `channel` | 76 | RF 信道 0-125（2400+ch MHz） |
| `data-rate` | "1m" | "250k" / "1m" / "2m" |
| `tx-power` | "0dbm" | "-18dbm" / "-12dbm" / "-6dbm" / "0dbm" |
| `address-width` | 5 | 3 / 4 / 5 字节 |
| `payload-mode` | "dynamic" | "dynamic" / "fixed" |
| `crc-mode` | "2-byte" | "1-byte" / "2-byte" |
| `tx-address` | [e7 e7 e7 e7 e7] | 收发地址（双方须一致） |

完整驱动 API 见 `drivers/nrf24l01p/nrf24l01p.h`。

## 目录结构

```
main.c                       -- TX/RX 角色逻辑
Kconfig                      -- 角色选择 choice
prj.conf                     -- 内核配置
CMakeLists.txt               -- 构建脚本
boards/stm32f407_daq.overlay -- 板级接线与 RF 参数
README.md                    -- 本文件
```
