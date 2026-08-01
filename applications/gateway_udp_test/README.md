# gateway_udp_test — gateway UDP 功能独立测试应用

运行在 STM32F407 (`stm32f407_xinxin` 板) 上的 Zephyr RTOS 应用，用于独立测试 gateway 的全部 UDP 网络功能。从 gateway 平移 UDP 双端口架构、配置命令、固件升级和持久化，去掉 nRF24 无线部分，改用 shell 命令模拟数据源。

## 与 gateway 的差异

| 项目 | gateway | gateway_udp_test |
|------|---------|------------------|
| 板 | nrf24_f103rct6 (STM32F103 + W5500 SPI 以太网) | stm32f407_xinxin (STM32F407 内置 MAC + 外部 PHY RMII) |
| 网络 | W5500 (SPI3) | STM32 内置 MAC (f407-mac snippet 启用) |
| nRF24 | 有 (SPI2, 数据透传) | **无** (shell 模拟数据源) |
| RF24 配置命令 | 0x07/0x08 | **去掉** |
| MCUBoot | swap-with-scratch, 外部 GD25Q80 | swap-with-scratch, 外部 W25Q128 |
| swd_recover | 有 (F1 JTAG/SWD 恢复) | **去掉** (F4 不需要) |
| 随机数源 | TEST_RANDOM_GENERATOR (伪随机) | ENTROPY_GENERATOR (F407 硬件 RNG) |
| 网络管理 | 无 | NET_MGMT + NET_MGMT_EVENT |

## 构建

```shell
# 激活 zephyr venv (Git Bash, 把 venv 的 Scripts 加入 PATH)
export PATH="/c/Users/jxwaz/venv/zephyr/Scripts:$PATH"

# 需要 f407-mac snippet 启用内置 MAC + PHY; -d 指定独立构建目录避免与其它 app 冲突
west build -b stm32f407/stm32f407xx/xinxin apps/applications/gateway_udp_test -d build_gut --sysbuild -- -S f407-mac

# 清理重建
west build -b stm32f407/stm32f407xx/xinxin apps/applications/gateway_udp_test -d build_gut --sysbuild -- -S f407-mac --pristine
```

> 在 workspace 根目录 (`C:\Users\jxwaz\code\app`) 下执行。默认构建目录被其它 app 占用，故用 `-d build_gut` 单独指定。

## 烧录

```shell
west flash -d build_gut
```

## 功能

### UDP 双端口

| 端口 | 用途 | 可配 | 默认 |
|------|------|------|------|
| **数据端口** | 数据帧收发 (shell 测试 / echo) | UDP_CMD_SET_NET 可改, 持久化 | 9090 |
| **配置端口** | 所有配置命令 + 固件升级 | 固定 | 9200 |

双端口均绑定 INADDR_ANY，支持广播收发，按子网判断单播/广播。

### 配置命令 (配置端口 9200)

与 gateway 协议完全一致 (分网络/RF24 两组, 各含 set/get; 本应用无 nRF24 硬件, RF24 字段仅持久化不应用). 业务命令从 0x12 起; 0x01-0x05 由 `udp_fw_upgrade` 库内部处理 (FW_START/DATA/END/GET_VERSION/REBOOT).

> 网络参数掩码固定 `255.255.255.0`，网关 = 设备 IP 末段改 1，均由固件派生不传输; config_port 固定 9200 不在响应中返回.

| 命令 | 格式 | 说明 |
|------|------|------|
| 0x01 | `[0x01][size 4B LE]` | 开始固件升级 (库处理, 回 `[0x01][1/0]`) |
| 0x02 | `[0x02][data ≤511B]` | 固件数据 (库处理, 回 `[0x02][offset 4B LE]`) |
| 0x03 | `[0x03][test 1B][crc 2B LE]` | 结束固件升级并重启 (库处理, 回 `[0x03][1/0]`) |
| 0x04 | `[0x04]` | 查询版本 (库处理, 回 APP_VERSION_STRING) |
| 0x05 | `[0x05]` | 重启 (库处理) |
| 0x12 | `[0x12][ip 4B][port 2B BE]` | 设置网络参数 (持久化; 回 6B `[ip 4B][port 2B]`) |
| 0x13 | `[0x13]` | 查询网络参数 (回 6B: `[ip 4B][port 2B]`) |
| 0x14 | `[0x14][rf24_ch 1B][rf24_addr 5B]` | 设置 RF24 信道/地址 (持久化, 无硬件不应用; 回 6B) |
| 0x15 | `[0x15]` | 查询 RF24 信道/地址 (回 6B: `[rf24_ch 1B][rf24_addr 5B]`) |
| 0x16 | `[0x16][mode 1B]` | 设置网络模式 (0=静态, 1=DHCP; 持久化, 重启生效; 回 1B 回显) |
| 0x17 | `[0x17]` | 查询网络模式 (回 1B: 0=静态, 1=DHCP) |

### Shell 命令

```
gut info              查看网络配置 (IP/掩码/网关/端口/RF24/echo)
gut send <id> <hex..> 发送数据帧 [帧ID 2B BE][payload]
gut ping [count=5]    发送 TEST_FRAME (0x777) 计数测试
gut echo [on|off]     开关数据端口回显 (收到数据原样回发)
gut ip <addr>         设置 IP (持久化, 重启生效)
gut port <port>       设置数据端口 (持久化, 重启生效)
```

## 测试方法

1. 用网线连接板子与 PC（或同一交换机），PC 配同网段 IP（如 192.168.1.10）。
2. 串口（USART1, 115200）观察日志，确认 `data port 9090 listening` 和 `config port 9200 listening`。
3. PC 上用工具向 `192.168.1.100:9200` 发配置命令（如 `[0x06]` 查询版本），验证回复。
4. shell 里 `gut echo on` 后，PC 向 `:9090` 发数据帧，验证原样回显。
5. shell 里 `gut send 0x777 aa bb cc`，PC 上监听 9090 验证收到 `[07 77 aa bb cc]`。

## 目录结构

```
gateway_udp_test/
  boards/
    stm32f407_xinxin.overlay  -- settings 分区指定
    rsa_mcuboot_2048.pem      -- MCUBoot 签名密钥 (复用自 gateway)
  include/gateway_udp_test.h  -- 公共定义 (帧ID, 配置参数, 接口)
  src/
    main.c                    -- 入口 + 静态 IP 初始化
    udp.c                     -- UDP 双端口 + 配置命令 + 固件升级
    persist.c                 -- settings 持久化 (FCB, cfg_partition)
    shell.c                   -- shell 测试命令
  CMakeLists.txt
  Kconfig                     -- 线程栈/优先级 (GUT_DATA_RX / GUT_UDP)
  prj.conf                    -- 网络栈 + MCUBoot + settings
  sysbuild.conf               -- MCUBoot sysbuild 配置
  sysbuild/mcuboot.conf
  VERSION
```
