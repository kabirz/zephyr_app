# 板级支持

本项目在 `boards/` 目录下维护多个 STM32 自定义板的板级支持包（含 devicetree、Kconfig、overlay）。

| 板名 | MCU | 核心 | 主频 | Flash / RAM | 典型应用 |
|------|-----|------|------|-------------|----------|
| `stm32f407` | STM32F407VETx | Cortex-M4F | 168 MHz | 512 KB / 192 KB | data_collect（DAQ） |
| `apollo_h743ii` | STM32H743IITx | Cortex-M7 | 480 MHz | 2 MB / 1 MB | data_collect（高性能） |
| `lora_f103rct6` | STM32F103RCT6 | Cortex-M3 | 72 MHz | 256 KB / 48 KB | mod_handler 手持控制器 |
| `laser_f103ret7` | STM32F103RET6 | Cortex-M3 | 72 MHz | 512 KB / 64 KB | laser_ctrl 激光控制 |
| `can_f103_rct6` | STM32F103RCT6 | Cortex-M3 | 72 MHz | 256 KB / 48 KB | CAN 通信应用 |
| `stm32f103_bluepill` | STM32F103C8T6 | Cortex-M3 | 72 MHz | 64 KB / 20 KB | 通用调试 / ZephyrLink |

## 板级限定符（qualifier）

部分板通过 `/` 路径限定不同硬件变体，例如：

- `f407vet6/stm32f407xx/daq` — data_collect 数据采集板配置
- `f407vet6/stm32f407xx/monv` — CANopen demo 配置

构建时在 `-b` 参数后追加限定符即可选择对应变体。

## 自定义板开发

新增自定义板时，在 `boards/<vendor>/` 下创建板级目录，包含：

- `<board>.yaml` — 板级元数据
- `<board>.dts` / `.overlay` — devicetree 描述
- `<board>.defconfig` — Kconfig 默认值

参考 [Zephyr Board Porting Guide](https://docs.zephyrproject.org/latest/hardware/porting/index.html)。
