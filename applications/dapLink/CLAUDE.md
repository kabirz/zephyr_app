# ZephyrLink - CMSIS-DAP Debug Probe

基于 Zephyr RTOS 的 CMSIS-DAP 调试探针固件，将 STM32 开发板变为全功能调试器。

## 项目概述

本项目实现 CMSIS-DAP v2 协议，通过 USB 同时提供：

| 功能 | 接口 | 说明 |
|------|------|------|
| CMSIS-DAP v2 | USB HID | 兼容 Keil/OpenOCD/pyOCD |
| 串口透传 | CDC ACM | 调试目标 UART 输出 |
| 拖放烧写 | MSC | 拖入 .hex/.bin 自动烧写目标（可选，需 ~32KB RAM） |
| 自动检测 | - | SWD IDCODE 识别目标芯片 |

## 硬件

### 目标板：stm32f103_mini（STM32F103RCT6）

- MCU: STM32F103RCT6（STM32F103Xc 系列）
- Flash: 256 KB
- RAM: 48 KB
- 时钟: 72 MHz（HSE 8MHz x9 PLL）
- USB: Full-Speed（PA11/PA12）

### 引脚分配

```
SWD 调试接口:
  PB13  --- SWCLK（推挽输出）
  PB14  --- SWDIO（双向）
  PB0   --- nRESET（开漏输出，低有效）
  GND   --- GND

串口透传（可选）:
  PA2 (USART2 TX)  ---> 目标 RX
  PA3 (USART2 RX)  <--- 目标 TX

USB:
  PA11  --- USB_DM
  PA12  --- USB_DP
```

## 构建系统

### 工具链

- **构建工具**: West（Zephyr 官方构建工具）
- **CMake**: >= 3.20
- **Zephyr SDK**: 包含 ARM 工具链

### 虚拟环境激活

```bash
source ~/code/venv/zephyr/bin/activate
```

激活后确认工具链可用：

```bash
west --version
echo $ZEPHYR_BASE
echo $ZEPHYR_SDK_INSTALL_DIR
```

### 构建命令

```bash
# 激活虚拟环境
source ~/code/venv/zephyr/bin/activate

# 进入项目目录
cd apps/applications/zephyrLink

# 构建（使用 stm32f103_mini 板级支持）
west build -b stm32f103_mini

# 构建并指定额外 overlay（用于自定义引脚配置）
west build -b stm32f103_mini -- -DDTC_OVERLAY_FILE=boards/stm32f103_mini_daplink.overlay

# 清理构建
west build -t clean

# 烧录（默认使用 openocd）
west flash

# 烧录（指定 runner）
west flash --runner pyocd
west flash --runner probe-rs

# 调试
west debug
```

### 构建输出

- 固件: `build/zephyr/zephyr.elf`
- 烧录镜像: `build/zephyr/zephyr.bin`
- 反汇编: `west build -t ram_report` / `west build -t rom_report`

## 项目结构

```
zephyrLink/
├── CMakeLists.txt           # Zephyr 项目配置
├── Kconfig                  # 项目级 Kconfig
├── prj.conf                 # 默认配置（USB、HID、CDC、MSC）
├── boards/
│   ├── stm32f103_mini_daplink.overlay   # DAPLink 引脚 DTS overlay
│   └── stm32f103_mini_daplink.conf      # 板级 Kconfig
├── src/
│   ├── main.c               # 主函数（初始化流程）
│   ├── dap/
│   │   ├── DAP.c            # CMSIS-DAP 命令处理（ARM 官方）
│   │   ├── DAP.h            # CMSIS-DAP 协议定义
│   │   ├── DAP_config.h     # HAL 配置（时钟、包大小、SWD/JTAG）
│   │   ├── SW_DP.c          # SWD 底层传输
│   │   ├── cmsis_compiler.h
│   │   └── dap_strings_compat.h
│   ├── usb/
│   │   ├── usbd_context.c   # USB 复合设备描述符（HID + CDC + MSC）
│   │   ├── usbd_context.h
│   │   ├── hid_dap.c        # HID-DAP 适配器（USB HID <-> CMSIS-DAP）
│   │   └── hid_dap.h
│   ├── hal/
│   │   ├── dap_gpio.c       # GPIO HAL（SWCLK/SWDIO/nRESET 引脚操作）
│   │   ├── dap_gpio.h
│   │   ├── dap_delay.c      # 延时 HAL（SWD 时序）
│   │   └── dap_delay.h
│   └── flash/               # MSC 拖放烧写（可选）
│       ├── flash_blob.h
│       ├── target_stm32f1.c # STM32F1 目标烧写算法
│       ├── target_stm32f4.c
│       └── ...
└── README.md
```

## 配置说明

### Kconfig 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CONFIG_DAPLINK_MSC_FLASH` | y | 启用 MSC 拖放烧写（需 ~32KB RAM） |
| `CONFIG_USB_DEVICE_STACK_NEXT` | y | 使用 Zephyr 新版 USB 栈 |
| `CONFIG_USBD_HID_SUPPORT` | y | HID 类（CMSIS-DAP） |
| `CONFIG_USBD_CDC_ACM_CLASS` | y | CDC ACM 类（串口透传） |
| `CONFIG_USBD_MSC_CLASS` | y | MSC 类（拖放烧写） |
| `CONFIG_UART_BRIDGE` | y | UART 桥接（CDC <-> 目标 UART） |

### 内存约束

STM32F103RCT6 有 48 KB RAM，MSC 功能需要约 32 KB RAM 用于内存磁盘映像。如果资源紧张：

```prj.conf
# 关闭 MSC 以节省 RAM
CONFIG_DAPLINK_MSC_FLASH=n
CONFIG_USBD_MSC_CLASS=n
CONFIG_DISK_ACCESS=n
```

## 代码规范

### 命名约定

- 文件命名: `snake_case.c/h`
- 函数命名: `dap_xxx()`（HAL 层）、`hid_xxx()`（USB 层）
- 变量命名: `snake_case`
- 宏命名: `UPPER_CASE`

### 关键依赖

- Zephyr USB 新栈（`CONFIG_USB_DEVICE_STACK_NEXT=y`）
- 新版 UDC 驱动（`CONFIG_UDC_DRIVER=y`）
- CMSIS-DAP 核心使用 ARM 官方源码，不要修改 DAP.c 中的协议逻辑

### 修改指南

- **修改引脚**: 编辑 `boards/stm32f103_mini_daplink.overlay` 中的 `swclk-gpios`、`swdio-gpios`、`nreset-gpios`
- **修改 USB VID/PID**: 编辑 `src/usb/usbd_context.c` 中的 `DAPLINK_USB_VID` 和 `DAPLINK_USB_PID`
- **添加新目标芯片支持**: 在 `src/flash/` 中添加 `target_xxx.c`，在 `CMakeLists.txt` 中注册
- **修改 SWD 时钟**: 编辑 `src/dap/DAP_config.h` 中的 `DAP_DEFAULT_SWJ_CLOCK`

## 调试技巧

```bash
# 查看内存使用
west build -t ram_report
west build -t rom_report

# 查看详细构建日志
west build -v

# RTT 日志（需硬件支持）
west debug
# 然后连接 J-Link RTT Viewer
```

## 参考

- [CMSIS-DAP 规范](https://arm-software.github.io/CMSIS_5/DAP/html/index.html)
- [Zephyr USB 文档](https://docs.zephyrproject.org/latest/connectivity/usb/)
- [Zephyr 构建系统](https://docs.zephyrproject.org/latest/build/west/index.html)
