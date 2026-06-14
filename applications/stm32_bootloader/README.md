# STM32 Bootloader

基于 Zephyr RTOS 的 STM32F103RCT6 Bootloader，支持 CAN/UART 双总线固件升级。

## 功能特性

- **双总线升级模式**
  - CAN 总线：250kbps，支持固件分包传输
  - UART 串口：115200bps，自定义帧格式，带 CRC16 校验

- **双入口升级模式**
  - 硬件：按键触发（Key1 按下）
  - 软件：应用固件设置升级标志（0x55AADEAD）

- **安全机制**
  - 固件校验（栈指针和复位向量范围检查）
  - Flash 擦除优化（按页擦除）
  - 升级完成自动跳转应用

## 硬件资源

| 外设 | 引脚 | 功能 |
|------|------|------|
| Key1 | PA13 | 进入升级模式按键（低电平有效） |
| LED1 | PC11 | 升级模式指示灯 |
| LED2 | PC12 | 错误指示灯 |
| CAN_RX | PA11 | CAN 接收 |
| CAN_TX | PA12 | CAN 发送 |
| UART1 | PA9/PA10 | 固件升级数据传输（115200bps） |

## Flash 地址划分

| 地址范围 | 大小 | 用途 |
|----------|------|------|
| 0x08000000 - 0x0800FFFF | 64KB | Bootloader |
| 0x08010000 - 0x0803FFFF | 192KB | 应用固件 |
| 0x0803F800 | 2KB | 升级标志区域 |

## 项目架构

```
stm32_bootloader/
├── boards/
│   └── can_f103rct6.overlay   # 板级 devicetree 覆盖
├── include/
│   ├── fw_can.h               # CAN 传输层接口
│   ├── fw_uart.h              # UART 传输层接口
│   └── fw_upgrade.h           # 固件升级核心接口
├── src/
│   ├── main.c                 # 主程序入口
│   ├── fw_can.c               # CAN 传输层实现
│   ├── fw_uart.c              # UART 传输层实现
│   └── fw_upgrade.c           # 固件升级核心逻辑
├── CMakeLists.txt             # 构建配置
├── prj.conf                   # Kconfig 配置
├── VERSION                    # 版本号
└── README.md                  # 本文档
```

## CAN 协议

### 消息格式

| CAN ID | 方向 | 说明 |
|--------|------|------|
| 0x101 | 上位机→Bootloader | 命令通道 |
| 0x102 | Bootloader→上位机 | 响应通道 |
| 0x103 | 上位机→Bootloader | 固件数据通道 |

### 命令定义

| 命令码 | 名称 | 参数 | 说明 |
|--------|------|------|------|
| 0 | START_UPDATE | 固件大小 | 开始升级，擦除 Flash |
| 1 | CONFIRM | 1=启动应用 | 确认升级，验证并跳转 |
| 2 | VERSION | - | 获取 Bootloader 版本 |
| 3 | REBOOT | - | 重启系统 |

### 响应定义

| 响应码 | 名称 | 说明 |
|--------|------|------|
| 0 | OFFSET | 当前接收偏移量 |
| 1 | UPDATE_SUCCESS | 升级成功 |
| 2 | VERSION | 版本号响应 |
| 3 | CONFIRM | 确认响应 |
| 4 | FLASH_ERROR | Flash 错误 |
| 5 | TRANSFER_ERROR | 传输错误 |

### 升级流程

```
上位机                      Bootloader
  │                             │
  ├──── 0x101: START_UPDATE ───→│  擦除 Flash
  │←──── 0x102: OFFSET(0) ──────┤
  │                             │
  ├──── 0x103: 固件数据 ───────→│  写入 Flash
  │←──── 0x102: OFFSET(64) ─────┤
  ├──── 0x103: 固件数据 ───────→│  写入 Flash
  │←──── 0x102: OFFSET(128) ────┤
  │          ...                │
  │←──── 0x102: UPDATE_SUCCESS ─┤
  │                             │
  ├──── 0x101: CONFIRM(1) ────→│  验证固件
  │←──── 0x102: CONFIRM ────────┤  跳转应用
  │                             │
```

## UART 协议

### 帧格式

```
+------+------+--------+----------+------+------+------+
| HEAD | TYPE | LENGTH |   DATA   | CRC16 |      | TAIL |
| 0xAA | 1B   | 2B BE  | 0-8 Bytes| 2B BE |      | 0x55 |
+------+------+--------+----------+------+------+------+
```

| 字段 | 大小 | 说明 |
|------|------|------|
| HEAD | 1字节 | 帧头，固定 0xAA |
| TYPE | 1字节 | 帧类型：0x01=命令，0x02=数据 |
| LENGTH | 2字节 | 数据长度（大端序） |
| DATA | 0-8字节 | 命令参数或固件数据 |
| CRC16 | 2字节 | CRC16-CCITT 校验（大端序） |
| TAIL | 1字节 | 帧尾，固定 0x55 |

## 编译

```bash
# 使用 Zephyr 虚拟环境
source C:\Users\jxwaz\venv\zephyr\Scripts\activate

# 首次编译（pristine）
west build -b can_f103rct6 applications/stm32_bootloader \
    --build-dir build_stm32_bootloader --pristine

# 增量编译
west build -b can_f103rct6 applications/stm32_bootloader \
    --build-dir build_stm32_bootloader

# 输出文件
# build_stm32_bootloader/zephyr/zephyr.bin
# build_stm32_bootloader/zephyr/zephyr.elf
```

## 使用说明

### 1. 进入升级模式

**方式一：按键触发**
- 上电时按下 Key1 按键（PA13，低电平有效）
- LED1 点亮表示进入升级模式

**方式二：应用固件触发**
- 应用固件向地址 `0x0803F800` 写入 `0x55AADEAD`
- 重启后 Bootloader 检测到标志，清除标志并进入升级模式
- Zephyr 应用可以使用 flash shell 命令：`flash write <flash_dev> <offset> 0x55aadead`

### 2. 固件升级

使用上位机工具通过 CAN 或 UART 发送固件数据，协议见上方 CAN 协议和 UART 协议章节。

## 应用固件要求

### 链接脚本修改

应用固件需要修改 Flash 起始地址：

```ld
/* STM32F103RCTx: 256KB Flash, 48KB RAM */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08010000, LENGTH = 192K
  RAM (xrw)   : ORIGIN = 0x20000000, LENGTH = 48K
}
```

在 Zephyr 中直接修改对应 board 的设备树文件，在 flash0 下修改：

```
reg = < 0x8000000 0x40000 >;
```

为：

```
reg = < 0x8010000 0x30000 >;
```

Bootloader 的大小为 64KB，根据需求自行确定。删除输出文件目录重新编译即可。

## 资源占用

| 资源 | 大小 | 占比 |
|------|------|------|
| FLASH | 23076 B | 8.80% |
| RAM | 16388 B | 33.34% |

## 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.0.0 | 2026-06 | 初始版本，支持 CAN/UART 升级 |

## 许可证

Apache-2.0
