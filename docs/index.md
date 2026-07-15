# Zephyr App

基于 [Zephyr RTOS](https://docs.zephyrproject.org/latest/) 的嵌入式应用集合，覆盖**数据采集、激光测距控制、手持无线控制器、无线接收器、CMSIS-DAP 调试探针**等场景，运行于 STM32 系列 MCU，集成 MCUboot 安全启动与 OTA 固件升级。

<div class="grid cards" markdown>

- :material-database:{ .lg .middle } **data_collect 数据采集**

    ---

    Modbus 服务器 + ADC 采集 + TCP/UDP 网络 + FTP/HTTP 文件服务，支持以太网上报与 OTA 升级。

    [:octicons-arrow-right-24: 查看文档](applications/data-collect.md)

- :material-radiator:{ .lg .middle } **laser_ctrl 激光控制**

    ---

    串口激光通信 + CAN 总线 + FPGA SPI + 外部 Flash，激光测距设备主控。

    [:octicons-arrow-right-24: 查看文档](applications/laser-ctrl.md)

- :material-gamepad-variant:{ .lg .middle } **mod_handler 手持控制器**

    ---

    操纵杆采集 + OLED 显示 + CAN/2.4G 双链路 + OTA 升级，激光测距远程操控。

    [:octicons-arrow-right-24: 查看文档](applications/mod-handler.md)

- :material-wifi:{ .lg .middle } **gateway 无线接收器**

    ---

    2.4G 无线接收 + CAN/UDP 双模式转发 + W5500 以太网 + Web 配置，手持控制器数据中转网关。

    [:octicons-arrow-right-24: 查看文档](applications/gateway.md)

- :material-usb:{ .lg .middle } **ZephyrLink 调试探针**

    ---

    CMSIS-DAP v2 + CDC ACM 串口透传，STM32 开发板变身全功能调试器。

    [:octicons-arrow-right-24: 查看文档](applications/zephyrlink.md)

</div>

## 特性概览

- **统一构建**：基于 west + CMake + sysbuild（MCUboot bootloader）
- **安全升级**：MCUboot swap-with-scratch，支持 CAN/FTP/smp 多通道 OTA
- **丰富外设**：ADC、CAN、nRF24L01+ 2.4G、SPI Flash、OLED、Ethernet、USB
- **多板支持**：STM32F1/F4/H7 系列自定义板
- **协议完善**：CANopen、Modbus、自定义 CAN/2.4G 二进制协议

## 快速开始

```bash
# 1. 获取源码
west init -m https://github.com/kabirz/zephyr_app app
cd app && west update

# 2. 构建 (以 data_collect 为例)
west build -b f407vet6/stm32f407xx/daq apps/applications/data_collect --sysbuild

# 3. 烧录
west flash
```

详见 [环境搭建](getting-started.md) 与 [构建与烧录](build-flash.md)。

## 仓库结构

```
apps/
├── applications/      # 应用源码 (data_collect / laser_ctrl / mod_handler / ...)
├── boards/            # 板级支持包 (STM32F1/F4/H7 自定义板)
├── docs/              # 本文档站点源
├── drivers/           # 自定义驱动
├── dts/               # 公共 devicetree
├── libs/              # 公共库
├── scripts/           # west 命令与辅助脚本
├── snippets/          # 可复用代码片段 (ftp_server / http_server / imgmgr ...)
└── tools/             # 镜像导出等工具脚本
```

许可证：Apache-2.0
