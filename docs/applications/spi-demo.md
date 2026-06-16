# spi_demo SPI 示例

Zephyr SPI 通信示例工程，演示 SPI 主机 / 从机传输 API 的用法，作为驱动开发与外设联调的参考模板。

## 目录结构

```
spi_demo/
├── master_slave/   # 主从同板示例
└── slave/          # SPI 从机示例
```

## 用途

- 学习 Zephyr [SPI Driver API](https://docs.zephyrproject.org/latest/hardware/peripherals/spi.html)
- 验证 SPI 引脚配置与传输时序
- 作为自定义 SPI 外设（传感器、显示屏、Flash 等）驱动的起点

!!! note "说明"
    spi_demo 为最小化教学示例，无独立 README 与 sysbuild 配置。参考各子目录 `CMakeLists.txt` 与 `prj.conf` 了解配置方式。
