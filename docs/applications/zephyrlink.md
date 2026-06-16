# ZephyrLink

基于 Zephyr RTOS 的 CMSIS-DAP v2 调试探针。将一块 STM32F103RCT6（stm32f103_mini）开发板变为全功能调试器，通过 USB 同时提供调试接口和串口透传。

## 功能

| 功能 | 接口 | 说明 |
|------|------|------|
| CMSIS-DAP v2 | USB Bulk | 兼容 OpenOCD / pyOCD / Keil / IAR |
| 串口透传 | CDC ACM | 调试目标 UART 输出，支持波特率/流控配置 |
| UART 桥接 | 内部 | CDC ACM ↔ USART2 双向透明传输 |

## 硬件

### 目标板：stm32f103_mini（STM32F103RCT6）

- MCU：STM32F103RCT6（STM32F103Xc 系列）
- Flash：256 KB
- RAM：48 KB
- 系统时钟：72 MHz（HSE 8 MHz × 9 PLL）
- USB：Full-Speed（PA11/PA12）

### 引脚分配

```
SWD 调试接口
  PA14  ── SWCLK（时钟输出）
  PA13  ── SWDIO（双向数据）
  PB0   ── nRESET（目标复位，低有效）
  GND   ── GND

串口透传
  PA2 (USART2 TX)  ──> 目标 RX
  PA3 (USART2 RX)  <── 目标 TX

USB
  PA11  ── USB_DM
  PA12  ── USB_DP

LED
  PC13  ── 用户 LED（低电平点亮，stm32f103_mini 板载）
```

### 与目标板连接

```
ZephyrLink (stm32f103_mini)          目标板
─────────────────────────           ──────
PA14 (SWCLK)              ────────> SWCLK
PA13 (SWDIO)              <───────> SWDIO
PB0  (nRESET)             ────────> nRST
PA2  (USART2 TX)          ────────> RX
PA3  (USART2 RX)          <──────── TX
GND                       ────────> GND
```

## 构建

### 环境要求

- Zephyr SDK（含 ARM 工具链）
- West 构建工具（在 Python 虚拟环境中）

### 激活虚拟环境

```bash
source ~/code/venv/zephyr/bin/activate
```

### 构建命令

```bash
cd apps/applications/zephyrLink

# 构建
west build -b stm32f103_mini

# 烧录
west flash

# 查看内存使用
west build -t rom_report
west build -t ram_report
```

### 构建产物

- 固件 ELF：`build/zephyr/zephyr.elf`
- 烧录镜像：`build/zephyr/zephyr.bin`

## 内存使用

| 资源 | 使用 | 总量 | 使用率 |
|------|------|------|--------|
| Flash | 70 KB | 256 KB | 27% |
| RAM | 19 KB | 48 KB | 39% |

## USB 设备信息

- **Vendor ID**：`0x0D28`（ARM Ltd.）
- **Product ID**：`0x0204`（CMSIS-DAP）
- **接口**：CMSIS-DAP v2（Bulk）+ CDC ACM（虚拟串口）

### 验证 USB 枚举

```bash
# Linux
lsusb | grep "0d28:0204"

# 查看接口详情
lsusb -v -d 0d28:0204 | grep -E "bInterfaceClass|bInterfaceSubClass|bInterfaceProtocol"
```

## 使用

### 调试目标

连接目标板后，通过 OpenOCD 使用：

```bash
openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg
```

通过 pyOCD 使用：

```bash
pyocd list          # 检测调试探针
pyocd flash firmware.bin
```

### 串口透传

CDC ACM 在主机上创建虚拟串口：

```bash
# Linux
ls /dev/ttyACM*

# minicom 连接
minicom -D /dev/ttyACM0 -b 115200
```

## 项目结构

```
zephyrLink/
├── CMakeLists.txt                  # Zephyr 项目配置
├── Kconfig                         # 项目级 Kconfig
├── prj.conf                        # 默认配置（USB + DAP + CDC）
├── boards/
│   ├── stm32f103_mini.overlay     # DTS overlay（SWD + CDC + UART bridge）
│   └── stm32f103_mini.conf         # 板级 Kconfig
├── src/
│   ├── main.c                      # 主函数（DAP + USB 初始化）
│   ├── webusb.h                    # WebUSB 描述符（预留）
│   ├── msosv2.h                    # MS OS 2.0 描述符（预留）
│   └── flash/                      # 拖放烧写子系统（预留）
│       ├── vfs_disk.c/h            # 虚拟 FAT16 磁盘
│       ├── flash_blob.h            # Flash 算法结构定义
│       ├── target_stm32f1.c        # STM32F1 flash 算法
│       ├── target_detect.c/h       # SWD IDCODE 目标检测
│       ├── swd_host.c/h            # SWD 底层操作
│       ├── intelhex.c/h            # Intel HEX 解析
│       └── flash_manager.c/h       # Flash 管理器
└── README.md
```

## 实现说明

### DAP 协议栈

使用 Zephyr 内置 CMSIS-DAP 子系统（`CONFIG_DAP` + `CONFIG_DAP_BACKEND_USB`），底层复用 ARM 官方 CMSIS-DAP 协议代码（V2.0.0）。

- SWD 传输通过 `zephyr,swdp-gpio` DTS 节点配置，由 `swdp_bitbang` 驱动实现 GPIO bit-bang
- USB 通信使用 Bulk endpoints（CMSIS-DAP v2 标准），兼容免驱动的 WinUSB（通过 MS OS 2.0 描述符，预留）

### USB 栈

使用 Zephyr 新版 USB 设备栈（`CONFIG_USB_DEVICE_STACK_NEXT`），通过 `usbd_register_all_classes()` 自动注册所有已配置的 USB 类：

- CMSIS-DAP Bulk（由 `CONFIG_DAP_BACKEND_USB` 自动注册）
- CDC ACM（由 `CONFIG_USBD_CDC_ACM_CLASS` 自动注册）
- MSC（由 `CONFIG_USBD_MSC_CLASS` 自动注册，预留）

### 串口桥接

通过 `zephyr,uart-bridge` DTS 节点将 CDC ACM 和 USART2 桥接，实现双向透明传输。主机通过 CDC ACM 设置的波特率和流控参数自动同步到 USART2。

## 扩展功能（预留）

以下功能代码已放置在 `src/flash/` 目录，但当前 `main.c` 和 `prj.conf` 中未启用：

- **MSC 拖放烧写**：虚拟 FAT16 磁盘（32 KB），拖入 `.bin` / `.hex` 文件自动烧写目标 MCU
- **WebUSB**：浏览器端 CMSIS-DAP 访问

启用方法：
1. `prj.conf` 中增加 `CONFIG_USBD_MSC_CLASS=y`、`CONFIG_DISK_ACCESS=y`、`CONFIG_HEAP_MEM_POOL_SIZE=4096`
2. `main.c` 中调用 `vfs_disk_init()` 并添加 BOS 描述符
3. `CMakeLists.txt` 中添加 `src/flash/*.c` 源文件

## 参考

- [CMSIS-DAP 规范](https://arm-software.github.io/CMSIS_5/DAP/html/index.html)
- [Zephyr USB 文档](https://docs.zephyrproject.org/latest/connectivity/usb/)
- [Zephyr DAP 子系统](https://docs.zephyrproject.org/latest/samples/subsys/dap/README.html)
- [DAPLink 项目](https://github.com/ARMmbed/DAPLink)
