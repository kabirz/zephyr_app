# CLAUDE.md -- gateway

## 项目愿景

gateway 是运行在 STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM) 上的 Zephyr RTOS 嵌入式应用，作为 **数据中转网关**，在 mod_handler（手持控制器）与上位机之间转发数据。

- **版本**: 0.1.0-dev
- **硬件平台**: nrf24_f103rct6 (与 mod_handler 共用同一 PCB，按外设贴装区分两套配置)
- **Bootloader**: MCUBoot (swap-with-scratch 模式)
- **无线模块**: Nordic nRF24L01+ (SPI2，2.4GHz，中断驱动)
- **网络芯片**: Wiznet W5500 (SPI2 以太网，**仅网关板配置**)
- **许可证**: Apache-2.0

---

## 双配置架构（核心）

同一份代码支持两种板级配置，通过 snippet 切换：

| 配置 | 硬件 | 网络 | 触发方式 |
|------|------|------|---------|
| **基线（默认）** | 手柄板（无 W5500） | 无 | 默认编译 |
| **网关板** | 网关板（带 W5500） | UDP | `WITH_GATEWAY_ETH=1` |

- **手柄板基线**：用手柄硬件充当无线接收端做功能测试，纯 nRF24 ↔ CAN 桥。
- **网关板**：完整网关，nRF24 ↔ CAN/UDP 双链路。

差异由三层协同控制：

1. **Kconfig `CONFIG_GW_NETWORKING`**（`Kconfig`，默认 `n`）：C 代码编译开关。启用时 `select` 整套网络栈（`NETWORKING`/`NET_SOCKETS`/`POSIX_API`/`ETH_W5500`/`NET_L2_ETHERNET`/`NET_ARP`/`ETH_DRIVER`/`NET_IPV4`/`NET_UDP`），`udp_forward.c` 参与编译；关闭时网络相关代码经 `#ifdef` 全部排除。
2. **snippet `gateway_eth`**（`snippets/gateway_eth/`）：启用网关板的"配方"，注入 `CONFIG_GW_NETWORKING=y`（conf）+ W5500 devicetree 节点/PA10 CS/chosen ethernet/linksw→PA2（overlay），叠加在基线 board overlay 之上（DT 后置覆盖前置）。
3. **基线**（`prj.conf` + `boards/nrf24_f103rct6.overlay`）：手柄板最小集，无网络。

**主干转发代码（`rf24.c` / `can_forward.c`）两种配置零差异共享**，网络分支全部 `#ifdef CONFIG_GW_NETWORKING` 守卫。

---

## 编译

```shell
# 手柄板（默认，无网络）
west build -b nrf24_f103rct6 applications/gateway --sysbuild --build-dir build/gateway

# 网关板（带 W5500 网络）
WITH_GATEWAY_ETH=1 west build -b nrf24_f103rct6 applications/gateway --sysbuild --build-dir build/gateway_eth

west flash
```

> `WITH_GATEWAY_ETH` 是**环境变量**而非 `-D` 参数：sysbuild 不转发命令行 `-D` 给子镜像 app，故 snippet 启用经环境变量在 app `CMakeLists.txt` 内追加（`if(DEFINED ENV{WITH_GATEWAY_ETH}) list(APPEND SNIPPET gateway_eth)`）。

---

## 硬件引脚分配

### 共用（两种配置）

| 功能 | 引脚 | 说明 |
|------|------|------|
| SPI2 SCK/MISO/MOSI | PB13/PB14/PB15 | SPI 共享总线 |
| nRF24 CS / CE / IRQ | PB12 / PA9 / PC6 | nRF24L01P |
| CAN RX/TX | PA11/PA12 | CAN1 250Kbps |
| 主电源 (5V) | PA8 | `mainpower`，软件使能 |
| CAN 电源 | PC7 | `canpower`，软件使能 |
| nRF24 电源 | PC9 | `rf24power`，软件使能 |

### 配置差异

| 功能 | 手柄板（基线） | 网关板（snippet） |
|------|---------------|-------------------|
| Link Switch | PA10 | PA2 |
| W5500 CS | — | PA10 |
| W5500 INT / RESET | — | PA1 / PB0 |

手柄板需 `main.c::gw_power_init()`（`PRE_KERNEL_2` 阶段）使能 mainpower/canpower/rf24power 三路，确保 nRF24/CAN 芯片在 Zephyr 驱动初始化前上电。

---

## 数据流

```
手柄板（基线，仅 CAN）:
  mod_handler --nRF24--> gateway --CAN--> host
  host --CAN--> gateway --nRF24--> mod_handler

网关板（+UDP，Link Switch 切 CAN/UDP）:
  mod_handler --nRF24--> gateway --CAN/UDP--> host
  host --CAN/UDP--> gateway --nRF24--> mod_handler
```

---

## CAN 协议

与 mod_handler 完全一致，帧 ID 定义在 `gateway.h::enum can_ids`：

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x1E3 (`HANDLER_STATE`) | 手柄→网关 | 手柄状态 (X/Y BE + 按键) |
| 0x263 (`OVERBREAK_LASER`) | 网关→手柄 | 超欠挖 + 激光测距 |
| 0x363 (`COORD_XY`) | 网关→手柄 | X/Y 坐标 |
| 0x463 (`COORD_Z`) | 网关→手柄 | Z 坐标 |
| 0x763 (`COBID_HEATBEAT`) | 手柄→网关 | 心跳 |
| 0x104 (`RF24_CONFIG_CMD`) | 平台→网关 | nRF24 配置命令（本地处理，不转发） |
| 0x105 (`RF24_CONFIG_RESP`) | 网关→平台 | nRF24 配置响应 |
| 0x106 (`NET_CONFIG_CMD`) | 平台→网关 | 网络配置命令（仅网关板，本地处理） |
| 0x107 (`NET_CONFIG_RESP`) | 网关→平台 | 网络配置响应 |
| 0x101/0x103 | 平台→网关 | 固件升级（不转发到 nRF24/UDP） |

---

## UDP 协议（仅网关板 snippet 启用时）

- **数据帧**: `[CAN ID 2B BE][payload]`（透传扫描仪数据）
- **命令帧**: `[0xAA][0x55][cmd 1B][data...]`（2 字节魔数头区分命令与数据）
- 配置命令 0x01~0x08（IP/掩码/网关/端口/查询/模式/RF24 信道/重启）
- 固件升级命令 0x10~0x12（开始/数据/结束重启）

详见 `README.md`。

---

## 关键设计决策

- **双配置零主干差异**：网络分支全部 `#ifdef CONFIG_GW_NETWORKING` 守卫，`rf24.c`/`can_forward.c` 两种配置共享同一份代码。
- **Kconfig select 链**：`GW_NETWORKING` 一个 bool `select` 整套网络栈，启用/关闭网络一行配置切换（C 代码层）。
- **snippet 叠加 overlay**：网关板 W5500 节点、SPI2 第二路 CS(PA10)、`linksw` 引脚(PA10→PA2)、`chosen ethernet` 经 snippet overlay 覆盖基线（DT 后置 overlay 覆盖前置属性）。
- **环境变量启用 snippet**：sysbuild 不转发命令行 `-D` 给子镜像，故用 `WITH_GATEWAY_ETH` 环境变量在 app `CMakeLists.txt` 内 `list(APPEND SNIPPET gateway_eth)`。
- **电源软件使能**：手柄板外设 GPIO 独立供电（与 mod_handler 一致），`gw_power_init` 在 `PRE_KERNEL_2` 拉高 mainpower/canpower/rf24power，先于设备驱动初始化。
- **mainpower 命名统一**：PA8 在手柄板(`handlerpower`)/网关板(`ethpower`)物理同引脚、同为主电源角色，基线统一为 `mainpower`，消除歧义。
- **STM32F103 无硬件 RNG**：网络栈随机源用 `CONFIG_TEST_RANDOM_GENERATOR`，放在 `gateway_eth.conf`（仅网络需要，基线不需要）。
- **SPI2 共享**：nRF24 与 W5500 共享 SPI2，不同 CS 区分（网关板 PB12 + PA10）。
- **CAN 固件升级**：使用 `libs/can_fw_upgrade` 共享库；`PLATFORM_RX`(0x101)/`FW_DATA_RX`(0x103) 不转发到 nRF24/UDP。
- **CAN 配置本地处理**：`RF24_CONFIG_CMD`(0x104)/`NET_CONFIG_CMD`(0x106) 本地处理不转发。
- **Web 已移除**：早期版本的 HTTP Web 配置/固件升级已删除，改为 UDP 命令协议（2 字节魔数头）。

---

## 源码结构

```
gateway/
  boards/
    nrf24_f103rct6.overlay   -- 基线板级覆盖（手柄板：nRF24+CAN+电源，无 W5500）
    nrf24_f103rct6.conf      -- 配对占位（不可删，内容可空；Zephyr kconfig 要求与 .overlay 配对存在）
  snippets/gateway_eth/       -- 网关板 snippet（W5500 网络）
    snippet.yml               -- append EXTRA_CONF_FILE + EXTRA_DTC_OVERLAY_FILE
    gateway_eth.conf          -- CONFIG_GW_NETWORKING=y + TEST_RANDOM_GENERATOR
    gateway_eth.overlay       -- W5500 节点 + PA10 CS + chosen ethernet + linksw PA2
  include/gateway.h           -- 公共定义（CAN ID 枚举、配置参数、gateway_params_t）
  src/
    main.c                    -- 入口 + gw_power_init + link switch + (net_init) + Shell
    rf24.c                    -- nRF24L01P 收发（UDP 分支条件编译）
    can_forward.c             -- CAN 转发 + (网络配置命令) + 固件升级(can_fw_upgrade)
    udp_forward.c             -- UDP 透传 + 配置 + 固件升级（仅 CONFIG_GW_NETWORKING 编译）
    config.c                  -- 配置加载（settings_load 封装）
    persist.c                 -- Settings 持久化（gw/* 键，FCB 后端）
  CMakeLists.txt              -- SNIPPET_ROOT 注册 + WITH_GATEWAY_ETH 条件 + udp_forward 条件编译
  Kconfig                     -- CONFIG_GW_NETWORKING + 线程栈/优先级
  prj.conf                    -- 基线配置（无网络）
  sysbuild.conf               -- MCUBoot sysbuild
  sysbuild/mcuboot.conf
  VERSION
```

### 线程清单

| 线程名 | 函数 | 栈/优先级 | 说明 |
|--------|------|-----------|------|
| `thread_rf24_rx` | `rf24_rx_thread` | 1024 / 8 | nRF24 接收，按模式转发 CAN/UDP |
| `thread_can_rx` | `can_rx_thread` | 1024 / 9 | CAN 接收，转发扫描仪数据到 nRF24 + 配置/固件升级 |
| `thread_udp_rx` | `udp_rx_thread` | 1024 / 10 | UDP 接收（仅网关板），命令/数据分发 |

---

## AI 使用指引

- **切换板级配置**：默认 = 手柄板（无网络）；`WITH_GATEWAY_ETH=1` = 网关板（W5500）。
- **网络相关代码必须 `#ifdef CONFIG_GW_NETWORKING` 守卫**，否则基线（手柄板）编译失败。判断标准："这个功能两种配置都需要吗？"——只网关板需要的，进 snippet；都需要的，进基线。
- **修改引脚**：手柄板/共用引脚在 `boards/nrf24_f103rct6.overlay`；网关板的覆盖或额外引脚（W5500、linksw PA2）在 `snippets/gateway_eth/gateway_eth.overlay`。
- **新增 Kconfig**：仅网关板需要 → `gateway_eth.conf`；两种都需要 → `prj.conf`。
- **`boards/nrf24_f103rct6.conf` 不可删除**：Zephyr kconfig 阶段要求它与 `.overlay` 配对存在（内容可空）。
- **修改 CAN 协议**：同步更新 `gateway.h::enum can_ids`，并与 mod_handler 保持一致（共用协议）。
- **`gateway_params_t gw_params`**（定义在 `main.c`）是全局共享状态：连接模式、RF24 配置、网络配置、运行标志。
- 与 mod_handler 的关系：共用同一 PCB 与 CAN 协议；mod_handler 是手柄端（采集+发送），gateway 是接收/中转端。
