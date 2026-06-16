# laser_ctrl 激光控制

激光测距设备主控应用，运行于 STM32F103RET6（`laser_f103ret7`）。通过串口与激光测距模块通信，经 CAN 总线与平台交互，支持 FPGA SPI 配置与 OTA 固件升级。

## 功能特性

- **串口激光通信** — 与激光测距模块的协议交互（`laser_serial.c`）
- **CAN 总线** — 与上层平台的数据交换（`can.c`）
- **FPGA SPI 配置** — 当 devicetree 存在 `spi_laser_fpga` 分区时自动编译（`fpga_spi.c`）
- **外部 Flash 存储** — 配置与参数持久化（`flash.c`）
- **固件升级** — MCUboot OTA（`firmware.c`）

## 构建

```bash
west build -b laser_f103ret7 apps/applications/laser_ctrl --sysbuild

# 启用 mcumgr shell 下载
west build -b laser_f103ret7 apps/applications/laser_ctrl --sysbuild \
    -Dlaser_ctrl_SNIPPET=imgmgr-shell
```

## Manifest 版本

激光应用使用独立的 Zephyr manifest：

```bash
west init -m https://github.com/kabirz/zephyr_app app --mf zephyr4_3.yml
cd app && west update

# 应用 Zephyr 补丁
git -C ../zephyr am "$(pwd)/patches/zephyr_*"
```

## 源码结构

```
laser_ctrl/
├── src/
│   ├── main.c           # 主程序
│   ├── laser_serial.c   # 激光模块串口通信
│   ├── can.c            # CAN 总线收发
│   ├── flash.c          # 外部 Flash 读写
│   ├── firmware.c       # OTA 固件升级
│   └── fpga_spi.c       # FPGA SPI 配置 (条件编译)
├── include/             # 公共头文件
├── dts/                 # devicetree
├── boards/              # 板级配置
└── sysbuild.conf        # MCUboot 配置
```

## 镜像导出

```bash
apps/tools/export_images.py
```

## 参考

- [构建与烧录](../build-flash.md)
- [CAN 总线升级](../can_flash.md)
