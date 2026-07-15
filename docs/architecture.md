# 架构概览

本页用 [Mermaid](https://mermaid.js.org/) 图表展示核心应用的系统架构、通信链路与关键流程，帮助快速建立整体认知。

## mod_handler 系统架构

mod_handler 采用 Zephyr 多线程架构，通过 `gloval_params_t` 全局结构体共享状态，`k_event` 跨线程通知，外围设备电源由 GPIO 统一管理。

### 线程模型

```mermaid
flowchart TD
    SYS["SYS_INIT 初始化<br/>power_init / gpio_init"]
    MAIN(["main 主循环<br/>事件驱动休眠/唤醒<br/>10 分钟无操作自动休眠"])

    CAN["CAN 收发线程<br/>priority 8 栈 2048<br/>消息收发 协议分发 OTA"]
    HEART["CAN 心跳线程<br/>priority 11 栈 1024<br/>800ms 心跳 + 0x1E3 上报"]

    RF24["2.4G 接收线程<br/>priority 8 栈 1024<br/>rf24_rx_msgq 取帧 按 CAN ID 分发"]
    IRQ["nRF24 IRQ 线程<br/>驱动内 priority 2 栈 1024<br/>中断底半部 排空 FIFO 处理 TX"]

    ADC["ADC 采集线程<br/>priority 7 角度 / 8 电压 栈 1024<br/>X/Y 角度周期 + 电压 200ms"]

    SYS --> MAIN
    SYS --> CAN
    SYS --> HEART
    SYS --> RF24
    SYS --> IRQ
    SYS --> ADC

    GP["global_params_t<br/>全局状态共享"]
    CAN -. 读写 .-> GP
    RF24 -. 读写 .-> GP
    ADC -. 读写 .-> GP
```

### CAN / 2.4G 通信链路

双通道冗余链路，默认 CAN，可通过 link_switch 按键（PA10）手动切换。切换时关闭对方电源并重新初始化。帧 ID 详见 [mod_handler 文档](applications/mod-handler.md)。

```mermaid
flowchart LR
    subgraph H["mod_handler 手柄"]
        CTRL["connect_type<br/>CAN_TYPE / RF24_TYPE"]
        SW["link_switch 按键<br/>PA10 手动切换链路"]
    end

    PLATFORM["平台 / 上位机"]
    GW["2.4G 无线接收器 (nRF24L01+)"]
    SCANNER["激光扫描仪"]

    CTRL <-->|"CAN 250Kbps"| PLATFORM
    CTRL <-->|"2.4G nRF24L01+ (硬件 ACK)"| GW
    GW --- SCANNER
    PLATFORM -.->|"扫描仪数据"| SCANNER
```

### OTA 固件升级流程

固件通过 CAN 总线分片传输，写入外部 SPI Flash（GD25Q80），由 MCUboot 以 swap-with-scratch 模式完成安全切换。

```mermaid
sequenceDiagram
    participant P as 平台
    participant M as mod_handler
    participant F as 外部 SPI Flash
    participant B as MCUboot

    P->>M: CAN 0x101 升级启动命令
    M->>F: 初始化写入上下文
    loop 分片传输
        P->>M: CAN 0x103 固件数据
        M->>F: 写入镜像分片
    end
    M->>P: CAN 0x102 验证确认
    Note over B: 重启后 MCUboot 执行
    B->>B: swap-with-scratch 安全切换
    B->>M: 运行新镜像
```

### 系统休眠 / 唤醒状态机

电源键触发或 10 分钟无操作自动休眠，关闭所有外设电源（CAN / 2.4G / 显示 / 手柄）；唤醒时重新上电并刷新显示。

```mermaid
stateDiagram-v2
    [*] --> Running
    Running --> Sleeping: 电源键 / 10 分钟无操作
    Sleeping --> Running: 电源键唤醒

    Running: 运行中
    Sleeping: 已休眠

    note right of Sleeping
        关闭 CAN/2.4G/显示/手柄电源
        rf24_deinit (POWER_DOWN + 断电)
        sleeping = true
    end note

    note right of Running
        重新上电所有外设
        2.4G 模式下 rf24_init
        200ms 后 reinit 显示
    end note
```

## data_collect 数据流

数据采集应用的多通道上报架构：

```mermaid
flowchart LR
    ADC["ADC 模拟量采集"]
    MB["Modbus 服务器<br/>原始 ADU"]

    FS["littlefs 文件系统"]
    FTP["FTP 服务器"]
    HTTP["HTTP 服务器"]
    NET["TCP/UDP 网络<br/>主动上报"]

    ADC --> FS
    ADC --> MB
    FS --> FTP
    FS --> HTTP
    ADC --> NET

    HOST["上位机 / 平台"]
    FTP -.->|"下载"| HOST
    HTTP -.->|"下载"| HOST
    MB <-->|"轮询"| HOST
    NET -.->|"推送"| HOST
```

## ZephyrLink USB 复合设备

CMSIS-DAP 调试探针的 USB 设备拓扑：

```mermaid
flowchart TD
    USB["Zephyr 新版 USB 栈<br/>CONFIG_USB_DEVICE_STACK_NEXT"]
    USB --> DAP["CMSIS-DAP v2<br/>USB Bulk<br/>兼容 Keil/OpenOCD/pyOCD"]
    USB --> CDC["CDC ACM<br/>虚拟串口透传"]
    USB --> MSC["MSC<br/>拖放烧写 可选"]

    DAP --> SWD["SWD 调试接口<br/>SWCLK/SWDIO/nRESET"]
    CDC --> UART["USART2 桥接<br/>目标 UART 透传"]
    MSC --> FLASH["目标 Flash<br/>hex/bin 自动烧写"]
```

!!! tip "深入阅读"
    各应用的完整协议、引脚、Shell 命令详见 [应用模块](applications/mod-handler.md)。
