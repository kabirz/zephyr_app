# nRF24L01P Zephyr 驱动设计

- **日期**：2026-07-06
- **状态**：已批准（设计阶段），待 spec 评审
- **作者**：kabirz
- **源驱动**：`~/Downloads/E01-Demo工程/E01_nRF24L01P_Demo/DRIVER/RF/`（亿佰特 Ebyte SDK，基于 STM8L10x 裸机 + IAR）
- **产出位置**：`apps/drivers/nrf24l01p/`、`apps/dts/bindings/nordic,nrf24l01p.yaml`、`apps/applications/nrf24l01p_demo/`

---

## 1. 背景

nRF24L01+ 是 Nordic Semiconductor 的 2.4GHz 射频收发芯片，通过 SPI 控制，使用 Enhanced ShockBurst 私有协议。Zephyr 主线**不包含**其驱动（它不兼容 IEEE 802.15.4 / Thread / BLE，无对应子系统）。

亿佰特 E01 模组提供的 SDK 基于 STM8L10x 裸机构建，硬件抽象层（CSN/CE/IRQ/SPI 字节交换）与具体 MCU 强耦合，无法直接在 Zephyr 上使用。本设计将其重写为符合 Zephyr 设备驱动模型的驱动，复用其芯片级知识（寄存器/命令/配置序列），替换硬件抽象层，并修正源驱动的若干缺陷。

源驱动分层：
- `nRF24L01P_REG.h` — SPI 命令、寄存器地址、位定义（**芯片级稳定知识，近乎原样复用**）
- `nRF24L01P.c/.h` — `L01_*` API（硬件抽象 → 寄存器读写 → 配置 → FIFO/Payload 收发）
- `APP/app.c` — 典型用法：TX/RX 切换 + 轮询 IRQ + 读/写 payload + 清 IRQ

## 2. 目标与非目标

**目标**
- 提供符合 Zephyr 设备驱动模型（Devicetree + `DEVICE_DT_INST_DEFINE`）的 nRF24L01P 驱动。
- 提供中层同步收发 API（`nrf24_send` / `nrf24_recv`，阻塞 + 超时），并保留底层寄存器入口。
- IRQ 由 GPIO 中断 + 信号量驱动（取代源驱动的轮询）。
- 提供 Devicetree binding 与 stm32f407 板级 overlay + 可编译的 sample。

**非目标（YAGNI）**
- 不接入 Zephyr net/ieee802154 子系统（协议不兼容）。
- 不实现 ztest 单元测试（模拟 SPI 收益有限，用户未要求）。
- 不支持多 pipe（当前仅 pipe0；API 预留扩展入口）。
- 不做异步/回调式 API（同步轮询已满足需求）。

## 3. 已确认的设计决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| API 抽象层级 | 中层同步收发（+ 底层 reg 入口） | 贴合源驱动 `app.c` 用法，消除重复状态机样板，YAGNI |
| IRQ 处理 | GPIO 下降沿中断 + `k_sem` | 省 CPU，标准 Zephyr 模式 |
| 交付范围 | 驱动 + binding + overlay + sample | 可直接 `west build` 验证 |

## 4. 目录与文件清单

```
apps/drivers/nrf24l01p/
├── CMakeLists.txt          # zephyr_library_amend() + zephyr_library_sources_ifdef(CONFIG_NRF24L01P ...)
├── Kconfig                 # CONFIG_NRF24L01P + CONFIG_NRF24L01P_INIT_PRIORITY
├── nrf24l01p.h             # 公共 API + 枚举 + struct nrf24_cfg
├── nrf24l01p_reg.h         # SPI 命令/寄存器/位定义（源自 REG.h，去类型依赖）
└── nrf24l01p.c             # 驱动实现

apps/dts/bindings/
└── nordic,nrf24l01p.yaml   # Devicetree binding（项目 dts_root=.）

apps/applications/nrf24l01p_demo/
├── CMakeLists.txt
├── prj.conf
├── main.c                  # Kconfig 选 TX/RX 角色
└── boards/stm32f407.overlay
```

**改动现有文件**（接入构建）：
- `apps/drivers/Kconfig`：在 `menu "Custom Device Drivers"` 内增加 `rsource "nrf24l01p/Kconfig"`。
- `apps/drivers/CMakeLists.txt`：增加 `add_subdirectory_ifdef(CONFIG_NRF24L01P nrf24l01p)`。

## 5. Devicetree binding（`nordic,nrf24l01p`）

作为 SPI 总线子节点。硬件连接 + RF 参数一并在 DT 描述，使能"配置即用"。

```yaml
description: Nordic nRF24L01+ 2.4GHz radio transceiver

compatible: "nordic,nrf24l01p"

include: [spi-device.yaml]

properties:
  ce-gpios:
    type: phandle-array
    required: true
    description: Chip Enable control GPIO.
  irq-gpios:
    type: phandle-array
    required: true
    description: IRQ output GPIO (active-low, falling edge).
  channel:
    type: int
    default: 76
    description: RF channel (0-125, 2400+channel MHz). 范围由驱动强制。
  data-rate:
    type: string
    default: "1m"
    enum: ["250k", "1m", "2m"]
  tx-power:
    type: string
    default: "0dbm"
    enum: ["-18dbm", "-12dbm", "-6dbm", "0dbm"]
  address-width:
    type: int
    default: 5
    enum: [3, 4, 5]
  payload-mode:
    type: string
    default: "dynamic"
    enum: ["dynamic", "fixed"]
  rx-payload-width:
    type: int
    default: 32
    description: Payload length when payload-mode = "fixed" (0-32, 由驱动强制).
  crc-mode:
    type: string
    default: "2-byte"
    enum: ["1-byte", "2-byte"]
  ard-us:
    type: int
    default: 4000
    description: Auto retransmit delay (µs). Mapped to nearest ARD step.
  arc:
    type: int
    default: 15
    description: Auto retransmit count (0-15, 由驱动强制).
  tx-address:
    type: uint8-array
    default: [0xE7, 0xE7, 0xE7, 0xE7, 0xE7]
    description: TX address (also RX pipe0 address for ACK).

> **实现注记**：Zephyr DT binding 不支持 `minimum`/`maximum` 约束关键字，故上例未使用；数值范围由驱动运行时强制（`channel & 0x7F`、`arc & 0x0F`、`rx_payload_width & 0x3F`）。`nrf24_configure` 接受枚举型 `struct nrf24_cfg`，实现内部通过 enum→string 反向映射（`data_rate_to_str` 等）构造临时 config 后调用 `apply_config_with`。
```

DT 用法示例：
```dts
&spi3 {
    status = "okay";
    pinctrl-0 = <&spi3_sck_pc10 &spi3_miso_pc11 &spi3_mosi_pc12>;
    cs-gpios = <&gpioc 13 GPIO_ACTIVE_LOW>;

    nrf24: nrf24l01p@0 {
        compatible = "nordic,nrf24l01p";
        reg = <0x0>;
        spi-max-frequency = <8000000>;
        ce-gpios = <&gpiob 0 GPIO_ACTIVE_HIGH>;
        irq-gpios = <&gpiob 1 GPIO_ACTIVE_LOW>;
        channel = <76>;
        data-rate = "1m";
        tx-power = "0dbm";
    };
};
```

## 6. 驱动数据结构

```c
/* dev->config：DT 实例化，const */
struct nrf24_config {
    struct spi_dt_spec bus;
    struct gpio_dt_spec ce;
    struct gpio_dt_spec irq;

    uint8_t channel;
    enum nrf24_data_rate data_rate;     /* 250K / 1M / 2M */
    enum nrf24_tx_power tx_power;       /* -18/-12/-6/0 dBm */
    uint8_t address_width;              /* 3/4/5 */
    enum nrf24_payload_mode payload_mode;
    uint8_t rx_payload_width;           /* fixed 模式 */
    enum nrf24_crc_mode crc_mode;       /* 1-byte/2-byte */
    uint8_t ard;                        /* ARD 原始 4-bit 编码：clamp(ard-us/250 - 1, 0, 15) */
    uint8_t arc;                        /* 自动重传次数 */
    uint8_t tx_addr[5];
};

/* dev->data：运行时 */
struct nrf24_data {
    struct k_sem irq_sem;               /* ISR give，send/recv take */
    struct k_mutex lock;                /* 串行化 SPI 访问 + 模式切换 */
    struct gpio_callback irq_cb;
    enum nrf24_mode mode;               /* 当前 RF 模式 */
    bool ready;
};
```

## 7. 公共 API（`nrf24l01p.h`）

```c
enum nrf24_mode         { NRF24_MODE_PTX, NRF24_MODE_PRX, NRF24_MODE_STANDBY, NRF24_MODE_POWER_DOWN };
enum nrf24_data_rate    { NRF24_RATE_250K, NRF24_RATE_1M, NRF24_RATE_2M };
enum nrf24_tx_power     { NRF24_PWR_18DBM, NRF24_PWR_12DBM, NRF24_PWR_6DBM, NRF24_PWR_0DBM };
enum nrf24_payload_mode { NRF24_PAYLOAD_DYNAMIC, NRF24_PAYLOAD_FIXED };
enum nrf24_crc_mode     { NRF24_CRC_1_BYTE, NRF24_CRC_2_BYTE };

/* 运行时配置：仅覆盖高频可变参数的子集。
 * payload_mode / rx_payload_width / ard / arc 等仅在 init 时从 DT 应用，不在运行时修改。
 * 传 NULL 表示按当前 DT 参数重新应用。 */
struct nrf24_cfg {
    uint8_t channel;
    enum nrf24_data_rate data_rate;
    enum nrf24_tx_power tx_power;
    enum nrf24_crc_mode crc_mode;
    uint8_t address_width;
    const uint8_t *tx_addr;             /* 可空，长度 = address_width */
};

/* 设备获取：struct device *rf = DEVICE_DT_GET(DT_NODELABEL(nrf24)); */

int  nrf24_configure      (const struct device *dev, const struct nrf24_cfg *cfg);
int  nrf24_send           (const struct device *dev, const void *buf, size_t len, k_timeout_t timeout);
int  nrf24_recv           (const struct device *dev, void *buf, size_t max_len, k_timeout_t timeout);
int  nrf24_set_mode       (const struct device *dev, enum nrf24_mode mode);
int  nrf24_start_rx       (const struct device *dev);   /* 便捷：set_mode(PRX) + CE high */
bool nrf24_rx_ready       (const struct device *dev);   /* 非阻塞查 STATUS.RX_DR */

/* 底层寄存器入口（power user；语义对齐源驱动 L01_ReadSingleReg 等） */
int  nrf24_read_reg       (const struct device *dev, uint8_t reg, uint8_t *val);
int  nrf24_write_reg      (const struct device *dev, uint8_t reg, uint8_t val);
int  nrf24_read_reg_multi (const struct device *dev, uint8_t reg, uint8_t *buf, size_t len);
int  nrf24_write_reg_multi(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len);
```

**关于 driver API 形式**：因 nRF24L01P 无对应 Zephyr subsystem，**不实现 `DEVICE_API` vtable**。驱动以 `DEVICE_DT_INST_DEFINE` 实例化（`POST_KERNEL` 自动 init），公共 API 以 `const struct device *` 入参的普通函数提供。这是 Zephyr 中"非标准子系统设备"的惯用法（参考 flash 模板 `flashfs.c` 的实例化方式，但不挂 flash vtable）。

## 8. 内部机制

### 8.1 SPI 传输
用 `spi_transceive_dt(&cfg->bus, &tx, &rx)`。CSN 由 SPI 控制器的 `cs-gpios` 自动管理（`spi-device.yaml` 提供）。nRF24L01P 要求"命令字节 + 数据字节"在**同一 CSN 低有效窗口内**，因此每次操作组成为单一 TX buffer 一次 transceive：

- 读单寄存器：TX=`[R_REGISTER|reg, 0xFF]`，RX=`[ignored, value]`
- 写单寄存器：TX=`[W_REGISTER|reg, value]`，RX 忽略
- 读多字节（如地址）：TX=`[cmd, 0xFF, …]`，RX=`[ignored, d0, d1, …]`
- 读 RX payload：先 `R_RX_PL_WID` 取长度，再 `R_RX_PAYLOAD` 读数据
- 写 TX payload：TX=`[W_TX_PAYLOAD, data0, data1, …]`

### 8.2 CE 控制
`gpio_pin_set_dt(&cfg->ce, val)`。发送时拉高 ≥10µs 触发传输；接收模式保持高。

### 8.3 IRQ 中断
`irq-gpios` 配置为输入 + 下降沿触发。回调函数**仅执行 `k_sem_give(&data->irq_sem)`**——不在 ISR 上下文做 SPI 事务（避免 SPI 驱动锁与 ISR 深度问题）。线程侧 `send`/`recv` 在 sem 上带超时等待，醒来后读 STATUS 区分 `TX_DS`/`MAX_RT`/`RX_DR`，处理完毕写 1 清位。

nRF24L01P 的 IRQ 为低有效电平型，触发后保持低直到 STATUS 对应位被写 1 清除——因此清位必须在处理完事件后执行。

### 8.4 互斥
`k_mutex` 保护：同一时刻仅允许一个收发操作；`send`/`recv`/`configure`/`set_mode` 均需持锁。

## 9. 初始化流程（`nrf24_init`）

1. `device_is_ready(spi/ce/irq)` 校验；失败返回 `-ENODEV`。
2. 配置 CE 输出（初始 low）；IRQ 输入 + 下降沿中断（`gpio_pin_interrupt_configure_dt(..., GPIO_INT_EDGE_TO_ACTIVE)`，配合 overlay 的 `GPIO_ACTIVE_LOW`）；`gpio_init_callback` + `gpio_add_callback`。
3. `k_sem_init(&irq_sem, 0, 1)`；`k_mutex_init(&lock)`。
4. 写 `CONFIG=0` 进入 powerdown。
5. 按 DT 参数顺序写寄存器：
   - `SETUP_AW`（地址宽度）
   - `SETUP_RETR = (ard_raw << 4) | arc`，其中 `ard_raw = clamp(ard-us / 250 - 1, 0, 15)`（datasheet：ARD 步进 250µs，250µs=0x0 … 4000µs=0xF）
   - `RF_CH`（信道）
   - `RF_SETUP`（速率 + 功率）
   - `EN_AA = ENAA_P0`
   - `EN_RXADDR = ERX_P0`
   - `CONFIG = EN_CRC | (CRCO if 2-byte) | PRIM_RX`
   - 载荷：dynamic → `DYNPD = DPL_P0`、`FEATURE = EN_DPL | EN_ACK_PAY`；fixed → `RX_PW_P0 = width`
   - `TX_ADDR`、`RX_ADDR_P0`（= tx_addr，PTX 用于接收 ACK）
6. `flush_tx`、`flush_rx`、清 `STATUS`（写 1 到 RX_DR|TX_DS|MAX_RT）。
7. 置 `ready=true`，返回 0。

## 10. 错误处理

| 场景 | 返回码 |
|---|---|
| `len == 0` 或 `> 32`（send/recv） | `-EINVAL` |
| SPI 事务失败 | `-EIO` |
| `MAX_RT`（达到最大重传，send） | `-EIO`，自动 `flush_tx` + 清 IRQ |
| 超时未收到事件 | `-EAGAIN` |
| 设备未就绪 / 未初始化 | `-EAGAIN` |
| configure 参数越界 | `-EINVAL` |

## 11. 对源驱动的修正

1. **源驱动 CRC 配置与注释不符**：源 `L01_Init` 注释写"Enable CRC,2 bytes"，却仅置 `EN_CRC`、未置 `CRCO`——按 datasheet 实际产生 **1 字节 CRC**（注释与实现不符）。**本驱动按 DT `crc-mode` 显式配置 CRCO 位**：`CONFIG = EN_CRC | (CRCO 仅当 2-byte)`，默认 2 字节。第 9 节 init 第 5 步与此一致。
2. **`SetDataRate` 污染寄存器**：源对 1Mbps 用 `mask |= ~((1<<RF_DR_LOW)|(1<<RF_DR_HIGH))`，将无关位全置 1 → 改为 `mask &= ~(...)` 仅清两位。
3. **自定义类型**：`INT8U`/`INT16U` → `uint8_t`/标准 `stdint` 类型。
4. **硬编码宏**：`INIT_ADDR`/`FIXED_PACKET_LEN`/`DYNAMIC_PACKET` → 移至 Devicetree 属性。
5. **轮询 IRQ**：busy-wait `GET_L01_IRQ()` → GPIO 中断 + 信号量。

## 12. sample 设计（`applications/nrf24l01p_demo`）

- `Kconfig`：`NRF24L01P_DEMO_ROLE`（`tx` / `rx`）。
- **TX**：每秒发送一个递增计数 payload（≤32 字节），`LOG_INF` 打印 `nrf24_send` 返回值。
- **RX**：`nrf24_start_rx()` 后循环 `nrf24_recv(rbuf, 32, K_SECONDS(5))`，收到即 `LOG_INF` 打印长度与内容。
- `prj.conf`：`CONFIG_NRF24L01P=y`、`CONFIG_SPI=y`、`CONFIG_GPIO=y`、`CONFIG_LOG=y`、console/serial 配置。
- `boards/stm32f407.overlay`：启用 `spi3`（PC10/PC11/PC12），`cs-gpios = PC13`，`ce-gpios = PB0`，`irq-gpios = PB1`。文件顶部注释说明：**spi1/2 已被 W25Q128 NOR flash 与 W5500 以太网占用；此为示例接线，请按实际硬件调整。**

## 13. 测试策略

1. **编译验证**：
   - `west build -b stm32f407 apps/applications/nrf24l01p_demo -- -DCONFIG_NRF24L01P_DEMO_ROLE_TX=y`（TX 角色）
   - 同上 RX 角色
   - 验证 DT binding 解析、驱动编译、sample 链接均通过。
2. **行为验证**（需硬件，人工）：两块 stm32f407 板（一 TX 一 RX）对跑，确认 LOG 中收发计数一致、payload 内容匹配。
3. **回归**：源驱动修正项（CRC、data rate 位操作）在 sample 运行时通过寄存器回读 + 对传结果间接验证。

## 14. 开放问题 / 未来扩展

- **多 pipe 支持**：当前仅 pipe0。`nrf24_set_rx_addr(pipe, addr, len)` 已在 API 路线图，后续按需补充 binding 的 per-pipe 属性。
- **ack payload / no-ack 发送**：源驱动提供 `L01_WriteRXPayload_InAck` / `L01_WriteTXPayload_NoAck`；当前 `nrf24_send` 默认走带 ACK 的 `W_TX_PAYLOAD`。如需 no-ack 或 ack-payload，后续在 API 增加 flag 参数。
- **发射模式自动往返**（transmit-then-receive）：当前需应用层显式 `set_mode` 切换。

---

## 附录 A：寄存器/命令定义来源

`nrf24l01p_reg.h` 直接移植自源 `nRF24L01P_REG.h`，去除对其余头文件的依赖，宏命名保持一致（`L01REG_*`、`R_REGISTER` 等），便于与源驱动及 datasheet 对照。
