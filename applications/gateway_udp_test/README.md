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
| **数据端口** | 数据帧收发 (shell 测试 / echo) | UDP_CMD_SET_PORT 可改, 持久化 | 9090 |
| **配置端口** | 所有配置命令 + 固件升级 | 固定 | 9200 |

双端口均绑定 INADDR_ANY，支持广播收发，按子网判断单播/广播。

### 配置命令 (配置端口 9200)

与 gateway 协议兼容（仅去掉 RF24 相关的 0x07/0x08）：

| 命令 | 格式 | 说明 |
|------|------|------|
| 0x01 | `[0x01][ip 4B]` | 设置 IP |
| 0x02 | `[0x02][mask 4B]` | 设置子网掩码 |
| 0x03 | `[0x03][gw 4B]` | 设置网关 |
| 0x04 | `[0x04][port 2B BE]` | 设置数据端口 |
| 0x05 | `[0x05]` | 查询配置 (返回 `[0x05][local_port 2B][remote_port 2B][config_port 2B]`, 6B net_test 格式; remote_port = local_port + 1) |
| 0x06 | `[0x06]` | 查询版本 |
| 0x09 | `[0x09]` | 重启 |
| 0x10 | `[0x10]` | 开始固件升级 |
| 0x11 | `[0x11][data...]` | 固件数据 |
| 0x12 | `[0x12]` | 结束固件升级并重启 |

### Shell 命令

```
gut info              查看网络配置 (IP/掩码/网关/端口/echo)
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
