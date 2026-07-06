# nRF24L01P Zephyr 驱动实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将亿佰特 Ebyte 的 nRF24L01P 裸机驱动重写为符合 Zephyr 设备驱动模型的驱动，并提供 Devicetree binding、stm32f407 overlay 与可编译的 sample。

**Architecture:** SPI + GPIO 中断抽象；中层同步收发 API（`nrf24_send`/`nrf24_recv` 阻塞+超时）+ 底层寄存器入口；以 `DEVICE_DT_INST_DEFINE` 实例化（无标准 subsystem，不挂 vtable）；IRQ 用 GPIO 下降沿中断 + `k_sem`，SPI 事务严格在单 CSN 窗口内。

**Tech Stack:** Zephyr 4.4、C99、Devicetree、Zephyr SPI/GPIO 子系统、west 构建。

---

## 执行修正摘要（实现后回填）

实际执行中对以下 plan 步骤做了修正（源于 Zephyr 4.4 实际行为）：

1. **board qualifier**：`-b stm32f407` → `-b stm32f407/stm32f407xx/daq`（板有 daq/monv/xinxin 变体，缺变体回退 stub.dts）。
2. **overlay 命名**：`stm32f407.overlay` → `stm32f407_daq.overlay`（匹配 `<board>_<variant>` 惯例）。
3. **app Kconfig**：自带 Kconfig 时必须 `source "Kconfig.zephyr"`，否则核心 symbol（SPI/GPIO/LOG 等）未加载。
4. **驱动 CMakeLists**：`zephyr_library_amend()` → `zephyr_library()`（apps/drivers 上下文无预建 `drivers__nrf24l01p` library 可 amend）。
5. **binding**：去掉 `minimum`/`maximum`（Zephyr DT binding 不支持，仅 default/description/const/enum/required/type 等），范围由驱动运行时强制（`& 0x7F`/`& 0x0F`/`& 0x3F`）。
6. **DT 字符串读取**：`DT_INST_PROP_STRING` → `DT_INST_PROP`（前者未定义，通用宏对 string 自动返回字面量）。
7. **config 存储**：枚举字段改为 `const char*`（`static const struct` 初始化不能调用 `parse_*` 函数），parse 推迟到 `nrf24_init`/`apply_config_with`；`nrf24_configure` 增补 enum→string 反向映射（`data_rate_to_str` 等）桥接。
8. **寄存器名**：`L01REG_EN_AA` → `L01REG_ENAA`（与源 REG.h 一致，无下划线）。
9. **SPI_DT_SPEC_INST_GET**：去掉 deprecated 的 delay 第 3 参数（Zephyr 4.4 改用 DT prop）。
10. **sample CMakeLists**：加 `target_include_directories(app PRIVATE .../drivers/nrf24l01p)` 以定位公共头。

最终验证：TX/RX 双角色 pristine 编译通过（`[182/182] zephyr.elf`），DT nrf24 节点正确生成（spi3@okay + nrf24l01p@0 全属性）。行为验证需硬件（两板对传）。

---

## 全局约定（覆盖 skill 默认）

- **不做 `git commit`**（用户输出样式规则：未主动要求不执行 git 操作）。计划中无 commit 步骤；如需提交由用户另行指示。
- **不做 host 单元测试 / TDD**（spec 决策：嵌入式 SPI+中断无法在 host 模拟）。验证手段：
  - 自动：`west build -b stm32f407 apps/applications/nrf24l01p_demo` 增量编译验证（Task 6 起每任务必做）。
  - 人工（需硬件）：sample TX/RX 对传 + 寄存器回读自检。
- 注释用中文，`LOG_*` 消息用英文。

## 文件结构

| 文件 | 职责 | 动作 |
|---|---|---|
| `apps/drivers/Kconfig` | 接入驱动子菜单 | Modify |
| `apps/drivers/CMakeLists.txt` | 接入子目录 | Modify |
| `apps/drivers/nrf24l01p/Kconfig` | `CONFIG_NRF24L01P` + 日志/优先级 | Create |
| `apps/drivers/nrf24l01p/CMakeLists.txt` | library 源 | Create |
| `apps/drivers/nrf24l01p/nrf24l01p_reg.h` | SPI 命令/寄存器/位定义 | Create |
| `apps/drivers/nrf24l01p/nrf24l01p.h` | 公共 API + 枚举 + `struct nrf24_cfg` | Create |
| `apps/drivers/nrf24l01p/nrf24l01p.c` | 驱动实现 | Create |
| `apps/dts/bindings/nordic,nrf24l01p.yaml` | DT binding | Create |
| `apps/applications/nrf24l01p_demo/CMakeLists.txt` | sample 构建 | Create |
| `apps/applications/nrf24l01p_demo/prj.conf` | sample 配置 | Create |
| `apps/applications/nrf24l01p_demo/Kconfig` | TX/RX 角色选择 | Create |
| `apps/applications/nrf24l01p_demo/main.c` | sample 逻辑 | Create |
| `apps/applications/nrf24l01p_demo/boards/stm32f407.overlay` | 板级接线 | Create |

---

## Task 1: 接入构建骨架

**Files:**
- Modify: `apps/drivers/Kconfig`
- Modify: `apps/drivers/CMakeLists.txt`
- Create: `apps/drivers/nrf24l01p/Kconfig`
- Create: `apps/drivers/nrf24l01p/CMakeLists.txt`

- [ ] **Step 1: 创建驱动目录的 Kconfig**

`apps/drivers/nrf24l01p/Kconfig`：
```kconfig
config NRF24L01P
	bool "Nordic nRF24L01+ 2.4GHz radio driver"
	default y
	depends on SPI
	help
	  启用 Nordic nRF24L01+ 射频收发器驱动（SPI + GPIO 中断）。

config NRF24L01P_INIT_PRIORITY
	int "nRF24L01P init priority"
	default 70
	depends on NRF24L01P
	help
	  nRF24L01P 设备初始化优先级。须晚于 SPI/GPIO 控制器（POST_KERNEL 内）。

config NRF24L01P_LOG_LEVEL
	int "nRF24L01P log level"
	default 3
	depends on NRF24L01P && LOG
	help
	  0=none 1=err 2=wrn 3=inf 4=dbg。
```

- [ ] **Step 2: 创建驱动目录的 CMakeLists.txt**

`apps/drivers/nrf24l01p/CMakeLists.txt`（仿 `drivers/flash/CMakeLists.txt`）：
```cmake
zephyr_library_amend()

zephyr_library_sources_ifdef(CONFIG_NRF24L01P nrf24l01p.c)
```

- [ ] **Step 3: 接入 drivers/Kconfig**

在 `apps/drivers/Kconfig` 的 `menu "Custom Device Drivers"` 内追加：
```kconfig
	rsource "nrf24l01p/Kconfig"
```
（与现有 `rsource "flash/Kconfig"` 并列）

- [ ] **Step 4: 接入 drivers/CMakeLists.txt**

在 `apps/drivers/CMakeLists.txt` 追加：
```cmake
add_subdirectory_ifdef(CONFIG_NRF24L01P nrf24l01p)
```

- [ ] **Step 5: 验证配置可被识别**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo -- -DCONFIG_NRF24L01P=y 2>&1 | grep -i nrf24 || true`
Expected: 此时 sample 尚不存在会报错，但 Kconfig 解析阶段应能识别 `CONFIG_NRF24L01P`。若提示 sample 找不到属正常（Task 6 创建）。

---

## Task 2: 寄存器定义头文件

**Files:**
- Create: `apps/drivers/nrf24l01p/nrf24l01p_reg.h`

- [ ] **Step 1: 写入寄存器/命令/位定义**

完整移植自源 `nRF24L01P_REG.h`，去除对 `bsp.h` 等的依赖，纯宏头文件。文件内容：

```c
/* nRF24L01+ SPI 命令、寄存器地址与位定义（移植自 Ebyte SDK nRF24L01P_REG.h） */
#ifndef NRF24L01P_REG_H
#define NRF24L01P_REG_H

/* SPI 命令 */
#define R_REGISTER          0x00
#define W_REGISTER          0x20
#define R_RX_PAYLOAD        0x61
#define W_TX_PAYLOAD        0xA0
#define FLUSH_TX            0xE1
#define FLUSH_RX            0xE2
#define REUSE_TX_PL         0xE3
#define R_RX_PL_WID         0x60
#define W_ACK_PAYLOAD       0xA8
#define W_TX_PAYLOAD_NOACK  0xB0
#define NOP                 0xFF

/* 寄存器地址 */
#define L01REG_CONFIG       0x00
#define L01REG_ENAA         0x01
#define L01REG_EN_RXADDR    0x02
#define L01REG_SETUP_AW     0x03
#define L01REG_SETUP_RETR   0x04
#define L01REG_RF_CH        0x05
#define L01REG_RF_SETUP     0x06
#define L01REG_STATUS       0x07
#define L01REG_OBSERVE_TX   0x08
#define L01REG_RPD          0x09
#define L01REG_RX_ADDR_P0   0x0A
#define L01REG_RX_ADDR_P1   0x0B
#define L01REG_RX_ADDR_P2   0x0C
#define L01REG_RX_ADDR_P3   0x0D
#define L01REG_RX_ADDR_P4   0x0E
#define L01REG_RX_ADDR_P5   0x0F
#define L01REG_TX_ADDR      0x10
#define L01REG_RX_PW_P0     0x11
#define L01REG_RX_PW_P1     0x12
#define L01REG_FIFO_STATUS  0x17
#define L01REG_DYNPD        0x1C
#define L01REG_FEATURE      0x1D

/* CONFIG 位 */
#define MASK_RX_DR          6
#define MASK_TX_DS          5
#define MASK_MAX_PT         4
#define EN_CRC              3
#define CRCO                2
#define PWR_UP              1
#define PRIM_RX             0

/* EN_AA / EN_RXADDR 位 */
#define ENAA_P0             0
#define ERX_P0              0

/* SETUP_AW */
#define AW_3BYTES           0x01
#define AW_4BYTES           0x02
#define AW_5BYTES           0x03

/* SETUP_RETR：ARD[7:4]=步进250us (val=us/250-1)，ARC[3:0]=重传次数 */
#define ARD_US(us)          (((((us) / 250) - 1) & 0x0F) << 4)

/* RF_SETUP 位 */
#define RF_DR_LOW           5
#define RF_DR_HIGH          3
#define PWR_N_18DB          (0x00 << 1)
#define PWR_N_12DB          (0x01 << 1)
#define PWR_N_6DB           (0x02 << 1)
#define PWR_N_0DB           (0x03 << 1)

/* STATUS 位 */
#define RX_DR               6
#define TX_DS               5
#define MAX_RT              4

/* DYNPD / FEATURE 位 */
#define DPL_P0              0
#define EN_DPL              2
#define EN_ACK_PAY          1

/* 便捷 */
#define IRQ_ALL             ((1 << RX_DR) | (1 << TX_DS) | (1 << MAX_RT))
#define NRF24_MAX_PAYLOAD   32

#endif /* NRF24L01P_REG_H */
```

- [ ] **Step 2: 语法检查**

Run: `gcc -fsyntax-only -x c apps/drivers/nrf24l01p/nrf24l01p_reg.h`
Expected: 无输出（通过）。

---

## Task 3: Devicetree binding

**Files:**
- Create: `apps/dts/bindings/nordic,nrf24l01p.yaml`

- [ ] **Step 1: 写入 binding**

内容与 spec 第 5 节一致：
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
    minimum: 0
    maximum: 125
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
    minimum: 0
    maximum: 32
  crc-mode:
    type: string
    default: "2-byte"
    enum: ["1-byte", "2-byte"]
  ard-us:
    type: int
    default: 4000
  arc:
    type: int
    default: 15
    minimum: 0
    maximum: 15
  tx-address:
    type: uint8-array
    default: [0xE7, 0xE7, 0xE7, 0xE7, 0xE7]
```

- [ ] **Step 2: YAML 语法检查**

Run: `python3 -c "import yaml,sys; yaml.safe_load(open('apps/dts/bindings/nordic,nrf24l01p.yaml')); print('OK')"`
Expected: `OK`

---

## Task 4: 公共 API 头文件

**Files:**
- Create: `apps/drivers/nrf24l01p/nrf24l01p.h`

- [ ] **Step 1: 写入公共 API**

```c
/** @file nrf24l01p.h
 *  @brief Nordic nRF24L01+ Zephyr 驱动公共 API。
 */
#ifndef NRF24L01P_H
#define NRF24L01P_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nrf24_mode {
    NRF24_MODE_PTX,
    NRF24_MODE_PRX,
    NRF24_MODE_STANDBY,
    NRF24_MODE_POWER_DOWN,
};

enum nrf24_data_rate {
    NRF24_RATE_250K,
    NRF24_RATE_1M,
    NRF24_RATE_2M,
};

enum nrf24_tx_power {
    NRF24_PWR_18DBM,
    NRF24_PWR_12DBM,
    NRF24_PWR_6DBM,
    NRF24_PWR_0DBM,
};

enum nrf24_payload_mode {
    NRF24_PAYLOAD_DYNAMIC,
    NRF24_PAYLOAD_FIXED,
};

enum nrf24_crc_mode {
    NRF24_CRC_1_BYTE,
    NRF24_CRC_2_BYTE,
};

/** 运行时配置：仅覆盖高频可变参数的子集。
 *  payload_mode / rx_payload_width / ard / arc 等仅在 init 时从 DT 应用。
 *  传 NULL 表示按当前 DT 参数重新应用。
 */
struct nrf24_cfg {
    uint8_t channel;
    enum nrf24_data_rate data_rate;
    enum nrf24_tx_power tx_power;
    enum nrf24_crc_mode crc_mode;
    uint8_t address_width;          /* 3/4/5，0 表示不改 */
    const uint8_t *tx_addr;         /* 可空，长度须 = address_width */
};

/** 应用/重新应用配置。cfg=NULL 时按 DT 参数重新应用。*/
int nrf24_configure(const struct device *dev, const struct nrf24_cfg *cfg);

/** 阻塞发送一帧。len ∈ [1,32]。成功返回 0。*/
int nrf24_send(const struct device *dev, const void *buf, size_t len, k_timeout_t timeout);

/** 阻塞/超时接收一帧。返回载荷字节数（>0），负值为错误码。*/
int nrf24_recv(const struct device *dev, void *buf, size_t max_len, k_timeout_t timeout);

/** 切换 RF 模式。*/
int nrf24_set_mode(const struct device *dev, enum nrf24_mode mode);

/** 便捷：进入 PRX 并拉高 CE 开始接收。*/
int nrf24_start_rx(const struct device *dev);

/** 非阻塞查询是否收到数据（读 STATUS.RX_DR）。*/
bool nrf24_rx_ready(const struct device *dev);

/* —— 底层寄存器入口（power user）—— */
int nrf24_read_reg(const struct device *dev, uint8_t reg, uint8_t *val);
int nrf24_write_reg(const struct device *dev, uint8_t reg, uint8_t val);
int nrf24_read_reg_multi(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len);
int nrf24_write_reg_multi(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NRF24L01P_H */
```

- [ ] **Step 2: 语法检查**

Run: `gcc -fsyntax-only -I/home/zed/code/rtos_app/zephyr/include -I/home/zed/code/rtos_app/zephyr/include/zephyr -x c apps/drivers/nrf24l01p/nrf24l01p.h 2>&1 | head`
Expected: 若 Zephyr 头路径不全可能有未找到警告，重点确认无本文件自身语法错误。后续 Task 6 的 west build 会完整校验。

---

## Task 5: 驱动骨架（可编译）

**Files:**
- Create: `apps/drivers/nrf24l01p/nrf24l01p.c`

目标：建立 config/data 结构体、DT 实例化宏、所有公共 API 的桩实现（返回 `-ENOSYS` 或 `0`），使驱动能编译链接。

- [ ] **Step 1: 写入骨架**

```c
/* nRF24L01+ Zephyr 驱动实现 */
#define DT_DRV_COMPAT nordic_nrf24l01p

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#include "nrf24l01p.h"
#include "nrf24l01p_reg.h"

LOG_MODULE_REGISTER(nrf24l01p, CONFIG_NRF24L01P_LOG_LEVEL);

/* mode 0, MSB, 8-bit */
#define NRF24_SPI_OPERATION  (SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

struct nrf24_config {
    struct spi_dt_spec bus;
    struct gpio_dt_spec ce;
    struct gpio_dt_spec irq;

    uint8_t channel;
    enum nrf24_data_rate data_rate;
    enum nrf24_tx_power tx_power;
    uint8_t address_width;
    enum nrf24_payload_mode payload_mode;
    uint8_t rx_payload_width;
    enum nrf24_crc_mode crc_mode;
    uint8_t ard;            /* ARD 原始 4-bit 编码 */
    uint8_t arc;
    uint8_t tx_addr[5];
};

struct nrf24_data {
    struct k_sem irq_sem;
    struct k_mutex lock;
    struct gpio_callback irq_cb;
    enum nrf24_mode mode;
    bool ready;
};

/* —— DT 字符串枚举解析 —— */
static enum nrf24_data_rate parse_data_rate(const char *s)
{
    if (!strcmp(s, "250k")) return NRF24_RATE_250K;
    if (!strcmp(s, "2m"))   return NRF24_RATE_2M;
    return NRF24_RATE_1M;
}

static enum nrf24_tx_power parse_tx_power(const char *s)
{
    if (!strcmp(s, "-18dbm")) return NRF24_PWR_18DBM;
    if (!strcmp(s, "-12dbm")) return NRF24_PWR_12DBM;
    if (!strcmp(s, "-6dbm"))  return NRF24_PWR_6DBM;
    return NRF24_PWR_0DBM;
}

static enum nrf24_payload_mode parse_payload_mode(const char *s)
{
    return (!strcmp(s, "fixed")) ? NRF24_PAYLOAD_FIXED : NRF24_PAYLOAD_DYNAMIC;
}

static enum nrf24_crc_mode parse_crc_mode(const char *s)
{
    return (!strcmp(s, "1-byte")) ? NRF24_CRC_1_BYTE : NRF24_CRC_2_BYTE;
}

#define NRF24_CFG_INIT(idx)                                                     \
static const struct nrf24_config nrf24_cfg_##idx = {                            \
    .bus = SPI_DT_SPEC_INST_GET(idx, NRF24_SPI_OPERATION, 0),                   \
    .ce  = GPIO_DT_SPEC_INST_GET(idx, ce_gpios),                                \
    .irq = GPIO_DT_SPEC_INST_GET(idx, irq_gpios),                               \
    .channel = DT_INST_PROP(idx, channel),                                      \
    .data_rate = parse_data_rate(DT_INST_PROP_STRING(idx, data_rate)),          \
    .tx_power  = parse_tx_power(DT_INST_PROP_STRING(idx, tx_power)),            \
    .address_width = DT_INST_PROP(idx, address_width),                          \
    .payload_mode = parse_payload_mode(DT_INST_PROP_STRING(idx, payload_mode)), \
    .rx_payload_width = DT_INST_PROP(idx, rx_payload_width),                    \
    .crc_mode = parse_crc_mode(DT_INST_PROP_STRING(idx, crc_mode)),             \
    .ard = (uint8_t)ARD_US(DT_INST_PROP(idx, ard_us)),                          \
    .arc = DT_INST_PROP(idx, arc),                                              \
};

/* 桩实现 —— Task 7-10 填充 */
int nrf24_configure(const struct device *dev, const struct nrf24_cfg *cfg)
{ ARG_UNUSED(dev); ARG_UNUSED(cfg); return -ENOSYS; }

int nrf24_send(const struct device *dev, const void *buf, size_t len, k_timeout_t timeout)
{ ARG_UNUSED(dev); ARG_UNUSED(buf); ARG_UNUSED(len); ARG_UNUSED(timeout); return -ENOSYS; }

int nrf24_recv(const struct device *dev, void *buf, size_t max_len, k_timeout_t timeout)
{ ARG_UNUSED(dev); ARG_UNUSED(buf); ARG_UNUSED(max_len); ARG_UNUSED(timeout); return -ENOSYS; }

int nrf24_set_mode(const struct device *dev, enum nrf24_mode mode)
{ ARG_UNUSED(dev); ARG_UNUSED(mode); return -ENOSYS; }

int nrf24_start_rx(const struct device *dev)
{ ARG_UNUSED(dev); return -ENOSYS; }

bool nrf24_rx_ready(const struct device *dev)
{ ARG_UNUSED(dev); return false; }

int nrf24_read_reg(const struct device *dev, uint8_t reg, uint8_t *val)
{ ARG_UNUSED(dev); ARG_UNUSED(reg); ARG_UNUSED(val); return -ENOSYS; }

int nrf24_write_reg(const struct device *dev, uint8_t reg, uint8_t val)
{ ARG_UNUSED(dev); ARG_UNUSED(reg); ARG_UNUSED(val); return -ENOSYS; }

int nrf24_read_reg_multi(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len)
{ ARG_UNUSED(dev); ARG_UNUSED(reg); ARG_UNUSED(buf); ARG_UNUSED(len); return -ENOSYS; }

int nrf24_write_reg_multi(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len)
{ ARG_UNUSED(dev); ARG_UNUSED(reg); ARG_UNUSED(buf); ARG_UNUSED(len); return -ENOSYS; }

static int nrf24_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    return 0;
}

#define NRF24_DEVICE(idx)                                        \
    NRF24_CFG_INIT(idx)                                          \
    static struct nrf24_data nrf24_data_##idx;                   \
    DEVICE_DT_INST_DEFINE(idx, nrf24_init, NULL,                 \
        &nrf24_data_##idx, &nrf24_cfg_##idx,                     \
        POST_KERNEL, CONFIG_NRF24L01P_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(NRF24_DEVICE)
```

- [ ] **Step 2: 此时编译会因 sample 缺失而失败**，Task 6 建好 sample 后统一编译。

---

## Task 6: sample 骨架 + overlay（建立 west build 验证链）

**Files:**
- Create: `apps/applications/nrf24l01p_demo/CMakeLists.txt`
- Create: `apps/applications/nrf24l01p_demo/prj.conf`
- Create: `apps/applications/nrf24l01p_demo/Kconfig`
- Create: `apps/applications/nrf24l01p_demo/main.c`
- Create: `apps/applications/nrf24l01p_demo/boards/stm32f407.overlay`

- [ ] **Step 1: sample CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(nrf24l01p_demo)

target_sources(app PRIVATE main.c)
```

- [ ] **Step 2: sample Kconfig（角色选择）**

```kconfig
choice NRF24L01P_DEMO_ROLE
	prompt "nRF24L01P demo role"
	default NRF24L01P_DEMO_ROLE_TX

config NRF24L01P_DEMO_ROLE_TX
	bool "Transmitter"

config NRF24L01P_DEMO_ROLE_RX
	bool "Receiver"
endchoice
```

- [ ] **Step 3: prj.conf**

```ini
CONFIG_NRF24L01P=y
CONFIG_SPI=y
CONFIG_GPIO=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3

# 角色：二选一（默认 TX；RX 改为下面注释项）
CONFIG_NRF24L01P_DEMO_ROLE_TX=y
# CONFIG_NRF24L01P_DEMO_ROLE_RX=y
```

- [ ] **Step 4: main.c 骨架（仅取设备并 LOG）**

```c
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include "nrf24l01p.h"

LOG_MODULE_REGISTER(nrf24_demo, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *const nrf24 = DEVICE_DT_GET(DT_NODELABEL(nrf24));

int main(void)
{
    if (!device_is_ready(nrf24)) {
        LOG_ERR("nRF24L01P device not ready");
        return 0;
    }
    LOG_INF("nRF24L01P ready");
    /* Task 11 填充 TX/RX 逻辑 */
    return 0;
}
```

- [ ] **Step 5: overlay（示例接线，spi1/2 已被占用，用 spi3）**

```dts
/* 示例接线：stm32f407 daq 板 spi1 接 W25Q128、spi2 接 W5500，故使用 spi3。
 * 引脚为示例，请按实际硬件调整。 */
&spi3 {
    status = "okay";
    pinctrl-0 = <&spi3_sck_pc10 &spi3_miso_pc11 &spi3_mosi_pc12>;
    pinctrl-names = "default";
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

> 注：`spi3_sck_pc10` 等需在 STM32 pinctrl dtsi 中存在；若该板 pinctrl 不支持这些 label，编译会在 pinctrl 阶段报错，按实际可用引脚调整即可（属板级适配，不阻塞驱动正确性）。

- [ ] **Step 6: 首次编译验证（编译链打通）**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo --pristine`
Expected: 编译通过（桩函数 + 骨架）。若 pinctrl label 不存在，按提示改 overlay 引脚后重试。**此步成功后即确立后续每任务的可编译验证基线。**

---

## Task 7: SPI 底层寄存器读写

**Files:**
- Modify: `apps/drivers/nrf24l01p/nrf24l01p.c`

在 Task 5 骨架基础上，把底层 reg 读写从桩替换为真实实现。

- [ ] **Step 1: 在 `LOG_MODULE_REGISTER` 之后、`struct nrf24_config` 之前插入 SPI/CE 辅助函数**

```c
/* —— 内部辅助（调用方须已持 data->lock）—— */
static inline const struct nrf24_config *get_cfg(const struct device *dev)
{
    return dev->config;
}

static inline struct nrf24_data *get_data(const struct device *dev)
{
    return dev->data;
}

/* 单次 SPI 收发（CSN 由控制器自动管理，命令+数据在同一 CSN 窗口）*/
static int nrf24_xfer(const struct device *dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
    const struct spi_dt_spec *bus = &get_cfg(dev)->bus;
    struct spi_buf tx_buf = { .buf = (void *)tx, .len = len };
    struct spi_buf rx_buf = { .buf = rx, .len = len };
    const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
    const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };
    int ret = spi_transceive_dt(bus, &txs, &rxs);
    if (ret < 0) {
        LOG_ERR("SPI transceive failed: %d", ret);
    }
    return ret;
}

/* 锁内寄存器读写 */
static int reg_read_locked(const struct device *dev, uint8_t reg, uint8_t *val)
{
    uint8_t tx[2] = { R_REGISTER | (reg & 0x1F), NOP };
    uint8_t rx[2] = { 0 };
    int ret = nrf24_xfer(dev, tx, rx, sizeof(tx));
    if (ret == 0) {
        *val = rx[1];
    }
    return ret;
}

static int reg_write_locked(const struct device *dev, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { W_REGISTER | (reg & 0x1F), val };
    return nrf24_xfer(dev, tx, NULL, sizeof(tx));
}

static int reg_read_multi_locked(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    uint8_t cmd = R_REGISTER | (reg & 0x1F);
    uint8_t nop[NRF24_MAX_PAYLOAD + 1] = { 0 };
    uint8_t rxb[NRF24_MAX_PAYLOAD + 1] = { 0 };
    if (len == 0 || len > NRF24_MAX_PAYLOAD) {
        return -EINVAL;
    }
    nop[0] = cmd;
    int ret = nrf24_xfer(dev, nop, rxb, len + 1);
    if (ret == 0) {
        memcpy(buf, &rxb[1], len);
    }
    return ret;
}

static int reg_write_multi_locked(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t txb[NRF24_MAX_PAYLOAD + 1];
    if (len == 0 || len > NRF24_MAX_PAYLOAD) {
        return -EINVAL;
    }
    txb[0] = W_REGISTER | (reg & 0x1F);
    memcpy(&txb[1], buf, len);
    return nrf24_xfer(dev, txb, NULL, len + 1);
}

/* 命令类（无数据寄存器）*/
static int cmd_locked(const struct device *dev, uint8_t cmd)
{
    return nrf24_xfer(dev, &cmd, NULL, 1);
}

static int read_rx_pl_wid_locked(const struct device *dev, uint8_t *wid)
{
    uint8_t tx[2] = { R_RX_PL_WID, NOP };
    uint8_t rx[2] = { 0 };
    int ret = nrf24_xfer(dev, tx, rx, sizeof(tx));
    if (ret == 0) {
        *wid = rx[1];
    }
    return ret;
}

static int read_rx_payload_locked(const struct device *dev, uint8_t *buf, uint8_t len)
{
    uint8_t nop[NRF24_MAX_PAYLOAD + 1];
    uint8_t rxb[NRF24_MAX_PAYLOAD + 1];
    memset(nop, NOP, sizeof(nop));
    nop[0] = R_RX_PAYLOAD;
    int ret = nrf24_xfer(dev, nop, rxb, len + 1);
    if (ret == 0) {
        memcpy(buf, &rxb[1], len);
    }
    return ret;
}

static int write_tx_payload_locked(const struct device *dev, const uint8_t *buf, uint8_t len)
{
    uint8_t txb[NRF24_MAX_PAYLOAD + 1];
    txb[0] = W_TX_PAYLOAD;
    memcpy(&txb[1], buf, len);
    return nrf24_xfer(dev, txb, NULL, len + 1);
}

/* CE 控制 */
static inline void ce_set(const struct device *dev, int val)
{
    gpio_pin_set_dt(&get_cfg(dev)->ce, val);
}
```

- [ ] **Step 2: 替换四个 public reg API（加锁包裹锁内实现）**

把 Task 5 中四个 `-ENOSYS` 桩替换为：
```c
int nrf24_read_reg(const struct device *dev, uint8_t reg, uint8_t *val)
{
    struct nrf24_data *d = get_data(dev);
    int ret;
    k_mutex_lock(&d->lock, K_FOREVER);
    ret = reg_read_locked(dev, reg, val);
    k_mutex_unlock(&d->lock);
    return ret;
}

int nrf24_write_reg(const struct device *dev, uint8_t reg, uint8_t val)
{
    struct nrf24_data *d = get_data(dev);
    int ret;
    k_mutex_lock(&d->lock, K_FOREVER);
    ret = reg_write_locked(dev, reg, val);
    k_mutex_unlock(&d->lock);
    return ret;
}

int nrf24_read_reg_multi(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    struct nrf24_data *d = get_data(dev);
    int ret;
    k_mutex_lock(&d->lock, K_FOREVER);
    ret = reg_read_multi_locked(dev, reg, buf, len);
    k_mutex_unlock(&d->lock);
    return ret;
}

int nrf24_write_reg_multi(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    struct nrf24_data *d = get_data(dev);
    int ret;
    k_mutex_lock(&d->lock, K_FOREVER);
    ret = reg_write_multi_locked(dev, reg, buf, len);
    k_mutex_unlock(&d->lock);
    return ret;
}
```

- [ ] **Step 3: 编译验证**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo`
Expected: 通过。

---

## Task 8: GPIO（CE/IRQ）+ 中断回调 + 信号量/互斥初始化

**Files:**
- Modify: `apps/drivers/nrf24l01p/nrf24l01p.c`

- [ ] **Step 1: 在 Task 7 的辅助函数后插入 IRQ 回调**

```c
static void nrf24_irq_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(pins);
    struct nrf24_data *d = CONTAINER_OF(cb, struct nrf24_data, irq_cb);
    /* ISR 内不做 SPI 事务，仅唤醒等待者 */
    k_sem_give(&d->irq_sem);
}

/* 检查 SPI/GPIO 设备就绪 + 配置方向/中断 */
static int nrf24_bus_init(const struct device *dev)
{
    const struct nrf24_config *cfg = get_cfg(dev);
    struct nrf24_data *d = get_data(dev);
    int ret;

    if (!spi_is_ready_dt(&cfg->bus)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&cfg->ce) || !gpio_is_ready_dt(&cfg->irq)) {
        LOG_ERR("CE/IRQ GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&cfg->ce, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }
    ret = gpio_pin_configure_dt(&cfg->irq, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }

    gpio_init_callback(&d->irq_cb, nrf24_irq_handler, BIT(cfg->irq.pin));
    ret = gpio_add_callback(cfg->irq.port, &d->irq_cb);
    if (ret < 0) {
        return ret;
    }
    /* IRQ 为 active-low（见 overlay irq-gpios），转 active 即下降沿 */
    ret = gpio_pin_interrupt_configure_dt(&cfg->irq, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        return ret;
    }
    return 0;
}
```

- [ ] **Step 2: 把 `nrf24_init` 桩替换为真实初始化（含 sem/mutex，寄存器配置留 Task 9）**

```c
static int nrf24_init(const struct device *dev)
{
    struct nrf24_data *d = get_data(dev);
    int ret;

    k_sem_init(&d->irq_sem, 0, 1);
    k_mutex_init(&d->lock);
    d->mode = NRF24_MODE_STANDBY;
    d->ready = false;

    ret = nrf24_bus_init(dev);
    if (ret < 0) {
        LOG_ERR("bus init failed: %d", ret);
        return ret;
    }

    /* Task 9：寄存器配置 */

    LOG_INF("nRF24L01P initialized");
    return 0;
}
```

- [ ] **Step 3: 编译验证**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo`
Expected: 通过。

---

## Task 9: 完整初始化序列（按 DT 写寄存器）

**Files:**
- Modify: `apps/drivers/nrf24l01p/nrf24l01p.c`

- [ ] **Step 1: 在 `nrf24_bus_init` 之后插入应用配置函数**

```c
static uint8_t data_rate_bits(enum nrf24_data_rate r)
{
    switch (r) {
    case NRF24_RATE_250K: return BIT(RF_DR_LOW);
    case NRF24_RATE_2M:   return BIT(RF_DR_HIGH);
    default:              return 0;                  /* 1M：两位均清 */
    }
}

static uint8_t tx_power_bits(enum nrf24_tx_power p)
{
    switch (p) {
    case NRF24_PWR_18DBM: return PWR_N_18DB;
    case NRF24_PWR_12DBM: return PWR_N_12DB;
    case NRF24_PWR_6DBM:  return PWR_N_6DB;
    default:              return PWR_N_0DB;
    }
}

static uint8_t addr_aw_bits(uint8_t aw)
{
    switch (aw) {
    case 3: return AW_3BYTES;
    case 4: return AW_4BYTES;
    default: return AW_5BYTES;
    }
}

/* 锁内：按 DT/cfg 写入全部 RF 寄存器。mode_cfg 为 PRIM_RX 位（PRX=1）。*/
static int apply_config_locked(const struct device *dev, bool prim_rx)
{
    const struct nrf24_config *cfg = get_cfg(dev);
    int ret;
    uint8_t aw = (cfg->address_width >= 3 && cfg->address_width <= 5) ?
                 cfg->address_width : 5;

    /* 进入 powerdown */
    ret = reg_write_locked(dev, L01REG_CONFIG, 0);
    if (ret < 0) return ret;

    ret = reg_write_locked(dev, L01REG_SETUP_AW, addr_aw_bits(aw));
    if (ret < 0) return ret;

    ret = reg_write_locked(dev, L01REG_SETUP_RETR, (uint8_t)(cfg->ard | (cfg->arc & 0x0F)));
    if (ret < 0) return ret;

    ret = reg_write_locked(dev, L01REG_RF_CH, cfg->channel & 0x7F);
    if (ret < 0) return ret;

    /* RF_SETUP：清速率/功率位后按 cfg 设 */
    uint8_t rf = data_rate_bits(cfg->data_rate) | tx_power_bits(cfg->tx_power);
    ret = reg_write_locked(dev, L01REG_RF_SETUP, rf);
    if (ret < 0) return ret;

    ret = reg_write_locked(dev, L01REG_EN_AA, BIT(ENAA_P0));
    if (ret < 0) return ret;

    ret = reg_write_locked(dev, L01REG_EN_RXADDR, BIT(ERX_P0));
    if (ret < 0) return ret;

    /* CONFIG：CRC（源驱动 bug 修正——显式 CRCO）+ PWR_UP + PRIM_RX */
    uint8_t config = BIT(EN_CRC) | BIT(PWR_UP);
    if (cfg->crc_mode == NRF24_CRC_2_BYTE) {
        config |= BIT(CRCO);
    }
    if (prim_rx) {
        config |= BIT(PRIM_RX);
    }
    ret = reg_write_locked(dev, L01REG_CONFIG, config);
    if (ret < 0) return ret;

    /* 载荷模式 */
    if (cfg->payload_mode == NRF24_PAYLOAD_DYNAMIC) {
        ret = reg_write_locked(dev, L01REG_DYNPD, BIT(DPL_P0));
        if (ret < 0) return ret;
        ret = reg_write_locked(dev, L01REG_FEATURE, BIT(EN_DPL) | BIT(EN_ACK_PAY));
        if (ret < 0) return ret;
    } else {
        ret = reg_write_locked(dev, L01REG_DYNPD, 0);
        if (ret < 0) return ret;
        ret = reg_write_locked(dev, L01REG_FEATURE, 0);
        if (ret < 0) return ret;
        ret = reg_write_locked(dev, L01REG_RX_PW_P0, cfg->rx_payload_width & 0x3F);
        if (ret < 0) return ret;
    }

    /* 地址：TX_ADDR 与 RX_ADDR_P0 同值（PTX 用 P0 收 ACK）*/
    ret = reg_write_multi_locked(dev, L01REG_TX_ADDR, cfg->tx_addr, aw);
    if (ret < 0) return ret;
    ret = reg_write_multi_locked(dev, L01REG_RX_ADDR_P0, cfg->tx_addr, aw);
    if (ret < 0) return ret;

    /* 清 FIFO 与 IRQ */
    cmd_locked(dev, FLUSH_TX);
    cmd_locked(dev, FLUSH_RX);
    reg_write_locked(dev, L01REG_STATUS, IRQ_ALL);

    /* 等待 PWR_UP/晶振稳定（datasheet：1.5ms）*/
    k_msleep(2);
    return 0;
}
```

- [ ] **Step 2: 在 DT 实例化宏里把 tx_addr 拷入 config**

把 Task 5 的 `NRF24_CFG_INIT` 宏的 `.arc = ...` 行之后补上 tx_addr 拷贝。由于 C 静态初始化不能用 memcpy，改用 `.tx_addr = { ... }` 形式，借助一个辅助宏从 DT 取数组：
```c
/* 替换 NRF24_CFG_INIT 中相关行，确保 tx_addr 来自 DT */
/* 在 NRF24_CFG_INIT 内，紧接 .arc = DT_INST_PROP(idx, arc), 之后： */
/* （DT_INST_PROP 对 uint8-array 返回 const uint8_t*；静态初始化需逐元素）
 *  这里采用 { DT_INST_PROP_BY_IDX(idx, tx_address, 0), ... } 的方式，5 元素： */
.tx_addr = {
    DT_INST_PROP_BY_IDX(idx, tx_address, 0),
    DT_INST_PROP_BY_IDX(idx, tx_address, 1),
    DT_INST_PROP_BY_IDX(idx, tx_address, 2),
    DT_INST_PROP_BY_IDX(idx, tx_address, 3),
    DT_INST_PROP_BY_IDX(idx, tx_address, 4),
},
```
> 若 DT `tx-address` 长度 < 5，`DT_INST_PROP_BY_IDX` 越界会编译报错——此时确保 DT 提供 5 字节（binding 默认即 5 字节）。address_width 控制实际写入芯片的字节数。

- [ ] **Step 3: 在 `nrf24_init` 中调用 apply_config**

把 Task 8 的 `nrf24_init` 里 `/* Task 9：寄存器配置 */` 注释替换为：
```c
    k_mutex_lock(&d->lock, K_FOREVER);
    ret = apply_config_locked(dev, true);   /* 默认上电进入 PRX 待命 */
    k_mutex_unlock(&d->lock);
    if (ret < 0) {
        LOG_ERR("apply_config failed: %d", ret);
        return ret;
    }

    /* 自检：回读 SETUP_AW 验证 SPI 链路 */
    uint8_t aw_rb = 0;
    reg_read_locked(dev, L01REG_SETUP_AW, &aw_rb);
    LOG_INF("nRF24L01P self-check SETUP_AW=0x%02x", aw_rb);

    d->ready = true;
    d->mode = NRF24_MODE_PRX;
    ce_set(dev, 1);   /* PRX：CE 保持高 */
```

- [ ] **Step 4: 编译验证**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo`
Expected: 通过。烧录后 LOG 应打印 `SETUP_AW=0x03`（5 字节地址编码）—— 若读到 0x00 或 0xFF，说明 SPI 接线/CS 问题，需排查硬件（人工验证）。

---

## Task 10: 中层收发 API

**Files:**
- Modify: `apps/drivers/nrf24l01p/nrf24l01p.c`

- [ ] **Step 1: 在 `apply_config_locked` 之后插入模式切换与收发实现**

```c
/* 锁内：仅切 PRIM_RX 位 + CE */
static int set_mode_locked(const struct device *dev, enum nrf24_mode mode)
{
    const struct nrf24_config *cfg = get_cfg(dev);
    struct nrf24_data *d = get_data(dev);
    uint8_t config;
    int ret = reg_read_locked(dev, L01REG_CONFIG, &config);
    if (ret < 0) return ret;

    bool was_powered = (config & BIT(PWR_UP)) != 0;

    config &= ~(BIT(PWR_UP) | BIT(PRIM_RX));
    switch (mode) {
    case NRF24_MODE_PRX:
        config |= BIT(PWR_UP) | BIT(PRIM_RX);
        break;
    case NRF24_MODE_PTX:
        config |= BIT(PWR_UP);
        break;
    case NRF24_MODE_STANDBY:
        config |= BIT(PWR_UP);
        break;
    case NRF24_MODE_POWER_DOWN:
        break;
    }
    ret = reg_write_locked(dev, L01REG_CONFIG, config);
    if (ret < 0) return ret;

    /* PWR_UP 0→1 需 ~1.5ms 晶振稳定（datasheet t pd2stdby）*/
    if (!was_powered && mode != NRF24_MODE_POWER_DOWN) {
        k_msleep(2);
    }

    /* CE：PRX 维持高；PTX/STANDBY/PWR_DOWN 拉低（PTX 由 send 脉冲）*/
    gpio_pin_set_dt(&cfg->ce, (mode == NRF24_MODE_PRX) ? 1 : 0);
    d->mode = mode;
    return 0;
}

int nrf24_set_mode(const struct device *dev, enum nrf24_mode mode)
{
    struct nrf24_data *d = get_data(dev);
    int ret;
    k_mutex_lock(&d->lock, K_FOREVER);
    ret = set_mode_locked(dev, mode);
    k_mutex_unlock(&d->lock);
    return ret;
}

int nrf24_start_rx(const struct device *dev)
{
    return nrf24_set_mode(dev, NRF24_MODE_PRX);
}

bool nrf24_rx_ready(const struct device *dev)
{
    uint8_t status = 0;
    if (nrf24_read_reg(dev, L01REG_STATUS, &status) < 0) {
        return false;
    }
    return (status & BIT(RX_DR)) != 0;
}

int nrf24_send(const struct device *dev, const void *buf, size_t len, k_timeout_t timeout)
{
    if (len == 0 || len > NRF24_MAX_PAYLOAD) {
        return -EINVAL;
    }
    struct nrf24_data *d = get_data(dev);
    int ret;

    k_mutex_lock(&d->lock, K_FOREVER);

    /* 排空陈旧 sem */
    while (k_sem_take(&d->irq_sem, K_NO_WAIT) == 0) { }

    ret = set_mode_locked(dev, NRF24_MODE_PTX);
    if (ret < 0) goto out;

    cmd_locked(dev, FLUSH_TX);
    ret = write_tx_payload_locked(dev, buf, (uint8_t)len);
    if (ret < 0) goto out;

    /* CE 高电平 ≥10us 触发发送 */
    ce_set(dev, 1);
    k_busy_wait(15);

    ret = k_sem_take(&d->irq_sem, timeout);
    if (ret < 0) {
        ce_set(dev, 0);
        ret = -EAGAIN;
        goto out;
    }

    uint8_t status = 0;
    reg_read_locked(dev, L01REG_STATUS, &status);
    if (status & BIT(MAX_RT)) {
        cmd_locked(dev, FLUSH_TX);
        ret = -EIO;
    } else {
        ret = 0;
    }
    reg_write_locked(dev, L01REG_STATUS, IRQ_ALL);   /* 清 IRQ 释放引脚 */
    ce_set(dev, 0);

out:
    k_mutex_unlock(&d->lock);
    return ret;
}

int nrf24_recv(const struct device *dev, void *buf, size_t max_len, k_timeout_t timeout)
{
    struct nrf24_data *d = get_data(dev);
    int ret;

    k_mutex_lock(&d->lock, K_FOREVER);

    /* 确保 PRX + CE 高 */
    if (d->mode != NRF24_MODE_PRX) {
        ret = set_mode_locked(dev, NRF24_MODE_PRX);
        if (ret < 0) goto out;
    }

    /* 先查是否已有数据到达（避免丢失已触发的 IRQ）*/
    uint8_t status = 0;
    reg_read_locked(dev, L01REG_STATUS, &status);
    if (!(status & BIT(RX_DR))) {
        /* 排空陈旧 sem 后等待新事件 */
        while (k_sem_take(&d->irq_sem, K_NO_WAIT) == 0) { }
        ret = k_sem_take(&d->irq_sem, timeout);
        if (ret < 0) {
            ret = -EAGAIN;
            goto out;
        }
        reg_read_locked(dev, L01REG_STATUS, &status);
    }

    if (!(status & BIT(RX_DR))) {
        ret = -EIO;
        goto out;
    }

    uint8_t plen = 0;
    const struct nrf24_config *cfg = get_cfg(dev);
    if (cfg->payload_mode == NRF24_PAYLOAD_DYNAMIC) {
        ret = read_rx_pl_wid_locked(dev, &plen);
        if (ret < 0) goto out;
        if (plen > NRF24_MAX_PAYLOAD) {
            cmd_locked(dev, FLUSH_RX);
            ret = -EIO;
            goto out_clr;
        }
    } else {
        plen = (cfg->rx_payload_width <= NRF24_MAX_PAYLOAD) ? cfg->rx_payload_width : NRF24_MAX_PAYLOAD;
    }

    if (plen > max_len) {
        plen = (uint8_t)max_len;
    }
    ret = read_rx_payload_locked(dev, buf, plen);
    if (ret < 0) goto out_clr;
    ret = plen;

out_clr:
    cmd_locked(dev, FLUSH_RX);
    reg_write_locked(dev, L01REG_STATUS, BIT(RX_DR));   /* 清 RX_DR 释放 IRQ */
out:
    k_mutex_unlock(&d->lock);
    return ret;
}

int nrf24_configure(const struct device *dev, const struct nrf24_cfg *cfg)
{
    struct nrf24_data *d = get_data(dev);
    const struct nrf24_config *dcfg = get_cfg(dev);
    struct nrf24_config tmp = *dcfg;

    if (cfg) {
        tmp.channel = cfg->channel;
        tmp.data_rate = cfg->data_rate;
        tmp.tx_power = cfg->tx_power;
        tmp.crc_mode = cfg->crc_mode;
        if (cfg->address_width >= 3 && cfg->address_width <= 5) {
            tmp.address_width = cfg->address_width;
        }
        if (cfg->tx_addr) {
            memcpy(tmp.tx_addr, cfg->tx_addr, tmp.address_width);
        }
    }

    int ret;
    k_mutex_lock(&d->lock, K_FOREVER);
    /* 临时切换 dev->config 不可行（const）；用栈副本写入：将临时值写回需去 const，
     * 因此仅支持基于 dcfg 默认 + cfg 覆盖，通过 apply_config_locked 直接传参。
     * 为保持简单，构造一个仅含差异的本地 config 指针：*/
    ret = apply_config_with(dev, &tmp, d->mode == NRF24_MODE_PRX);
    k_mutex_unlock(&d->lock);
    return ret;
}
```

- [ ] **Step 2: 把 `apply_config_locked` 重构为接受任意 config 指针**

将原 `apply_config_locked(const struct device *dev, bool prim_rx)` 改为 `apply_config_with(const struct device *dev, const struct nrf24_config *cfg, bool prim_rx)`：把函数体内所有 `get_cfg(dev)` 替换为参数 `cfg`（语义不变，只是数据源可变）。`nrf24_init` 与 `set_mode_locked` 中不受影响（set_mode_locked 不调用 apply）。`nrf24_init` 中的调用改为 `apply_config_with(dev, get_cfg(dev), true)`。

> 实现提示：即在 `apply_config_locked` 签名上加 `const struct nrf24_config *cfg` 参数，函数体首行删除 `const struct nrf24_config *cfg = get_cfg(dev);`（改为用参数）。其余代码完全不变。

- [ ] **Step 3: 编译验证**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo`
Expected: 通过。

---

## Task 11: sample TX/RX 角色逻辑

**Files:**
- Modify: `apps/applications/nrf24l01p_demo/main.c`

- [ ] **Step 1: 替换 main.c 为完整 TX/RX 逻辑**

```c
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "nrf24l01p.h"

LOG_MODULE_REGISTER(nrf24_demo, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *const nrf24 = DEVICE_DT_GET(DT_NODELABEL(nrf24));

#ifdef CONFIG_NRF24L01P_DEMO_ROLE_TX
static void run_tx(void)
{
    uint8_t seq = 0;
    uint8_t payload[4];

    while (1) {
        payload[0] = seq++;
        payload[1] = 0xA5;
        payload[2] = 0x5A;
        payload[3] = (uint8_t)(0xFF - payload[0]);

        int ret = nrf24_send(nrf24, payload, sizeof(payload), K_MSEC(100));
        if (ret == 0) {
            LOG_INF("TX seq=%u ok", payload[0]);
        } else {
            LOG_WRN("TX seq=%u failed: %d", payload[0], ret);
        }
        k_sleep(K_SECONDS(1));
    }
}
#else
static void run_rx(void)
{
    uint8_t rbuf[32] = {0};
    int ret = nrf24_start_rx(nrf24);
    if (ret < 0) {
        LOG_ERR("start_rx failed: %d", ret);
        return;
    }

    while (1) {
        int n = nrf24_recv(nrf24, rbuf, sizeof(rbuf), K_SECONDS(5));
        if (n > 0) {
            LOG_INF("RX len=%d: [%02x %02x %02x %02x]",
                    n, rbuf[0], rbuf[1], rbuf[2], rbuf[3]);
        } else if (n == -EAGAIN) {
            LOG_INF("RX timeout");
        } else {
            LOG_WRN("RX error: %d", n);
        }
    }
}
#endif

int main(void)
{
    if (!device_is_ready(nrf24)) {
        LOG_ERR("nRF24L01P device not ready");
        return 0;
    }
    LOG_INF("nRF24L01P ready, role=%s",
            IS_ENABLED(CONFIG_NRF24L01P_DEMO_ROLE_TX) ? "TX" : "RX");

#ifdef CONFIG_NRF24L01P_DEMO_ROLE_TX
    run_tx();
#else
    run_rx();
#endif
    return 0;
}
```

- [ ] **Step 2: 编译 TX 角色**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo --pristine -- -DCONFIG_NRF24L01P_DEMO_ROLE_TX=y`
Expected: 通过。

- [ ] **Step 3: 编译 RX 角色**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo --pristine -- -DCONFIG_NRF24L01P_DEMO_ROLE_RX=y`
Expected: 通过。

---

## Task 12: 最终验证与收尾

- [ ] **Step 1: 全量 pristine 编译（两角色）**

```bash
west build -b stm32f407 apps/applications/nrf24l01p_demo -d build/nrf24_tx --pristine -- -DCONFIG_NRF24L01P_DEMO_ROLE_TX=y
west build -b stm32f407 apps/applications/nrf24l01p_demo -d build/nrf24_rx --pristine -- -DCONFIG_NRF24L01P_DEMO_ROLE_RX=y
```
Expected: 均成功生成 `zephyr.bin`。

- [ ] **Step 2: DT binding 解析确认**

Run: `west build -b stm32f407 apps/applications/nrf24l01p_demo -d build/nrf24_tx --target hierarchy 2>/dev/null || grep -r "nordic,nrf24l01p" build/nrf24_tx/zephyr/zephyr.dts 2>/dev/null`
Expected: 生成的 `zephyr.dts` 中存在 `compatible = "nordic,nrf24l01p"` 节点，且 `status = "okay"`。

- [ ] **Step 3: 人工行为验证（需硬件）**

两块 stm32f407 板 + 两片 nRF24L01P 模组，相同信道/地址/速率：
1. 一板烧 RX：`west flash -d build/nrf24_rx`，串口应每 5s 打 `RX timeout`。
2. 另一板烧 TX：`west flash -d build/nrf24_tx`，每秒打印 `TX seq=N ok`。
3. RX 端应每秒收到 `RX len=4: [seq a5 5a ..]`，seq 递增。
4. init 阶段两板均打印 `self-check SETUP_AW=0x03`。

若 SETUP_AW 读回 0x00/0xFF：SPI 接线/CS 极性/电源问题，排查硬件。

- [ ] **Step 4: 文档收尾**

在 `apps/docs/` 下新增/更新驱动使用说明（可选）：引用本设计文档与 sample 路径，列出接线表与配置项。

---

## 验证策略汇总

| 验证类型 | 覆盖 | 自动化 |
|---|---|---|
| 编译（每任务） | 全部代码 + DT 解析 + 链接 | 自动（west build） |
| 寄存器自检 | SPI 链路连通性 | 半自动（LOG 输出，人工查看） |
| TX/RX 对传 | 收发功能正确性 | 人工（需两板硬件） |
| 源驱动 bug 修正 | CRC/DataRate 行为 | 间接（对传成功即验证） |
