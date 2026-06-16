# data_collect 数据采集

数据采集应用，运行于 STM32F407（DAQ 板）或 STM32H743，提供 **Modbus 服务器、ADC 采集、网络上报与文件服务**，集成 MCUboot OTA 升级能力。

## 功能特性

- **Modbus 服务器** — 原始 ADU 模式（`CONFIG_MODBUS_RAW_ADU`），供上位机轮询采集数据
- **ADC 采集** — 多通道模拟量采集（电压 / 电流 / 传感器信号）
- **网络通信** — IPv4 + TCP/UDP/Socket，支持以太网数据上报
- **文件服务** — FTP 服务器 + HTTP 服务器（基于 littlefs 文件系统）
- **OTA 升级** — imgmgr 镜像管理 + MCUboot sysbuild

## 构建

```bash
# STM32F407 数据采集板
west build -b f407vet6/stm32f407xx/daq apps/applications/data_collect --sysbuild

# STM32H743 高性能板
west build -b apollo_h743ii apps/applications/data_collect --sysbuild
```

## 关键配置（prj.conf）

```conf
# 日志
CONFIG_LOG=y
CONFIG_LOG_OUTPUT_FORMAT_DATE_TIMESTAMP=y

# Modbus 服务器
CONFIG_MODBUS=y
CONFIG_MODBUS_ROLE_SERVER=y
CONFIG_MODBUS_RAW_ADU=y

# ADC
CONFIG_ADC=y

# 网络
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
```

## 代码片段（snippets）

data_collect 通过 snippet 复用通用功能，定义于应用内 `snippets/`：

| Snippet | 说明 |
|---------|------|
| `shell_en` | 启用 shell 调试 |
| `littlefs_flash` | littlefs 文件系统 |
| `imgmgr` | MCUboot 镜像管理 |
| `ftp_server` | FTP 文件服务器 |
| `http_server` | HTTP 文件服务器 |

## 源码结构

```
data_collect/
├── src/
│   ├── main.c         # 主程序
│   ├── time.c         # RTC 时间 (CONFIG_RTC)
│   ├── fs.c           # 文件系统 (CONFIG_FAT_FILESYSTEM_ELM)
│   └── modbus/        # Modbus 实现
├── boards/            # 板级配置
├── snippets/          # 可复用代码片段
├── sysbuild.conf      # MCUboot sysbuild 配置
├── prj.conf           # 内核与功能配置
└── Kconfig            # 应用级配置项
```

## 数据上报

采集数据可通过以下方式获取：

- **Modbus 轮询** — 上位机作为 Modbus 客户端读取
- **FTP/HTTP 下载** — 数据落盘 littlefs，主机通过 [FTP](../ftp_download.md) 下载 `.raw` 文件
- **网络上报** — TCP/UDP 主动推送
