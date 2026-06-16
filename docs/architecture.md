# 架构概览

本页用 [Mermaid](https://mermaid.js.org/) 图表展示核心应用的系统架构、通信链路与关键流程，帮助快速建立整体认知。

## mod_handler 系统架构

mod_handler 采用 Zephyr 多线程架构，通过 `gloval_params_t` 全局结构体共享状态，`k_event` 跨线程通知，外围设备电源由 GPIO 统一管理。

### 线程模型

```mermaid
flowchart TD
    SYS["SYS_INIT 初始化<br/>power_init (PRE_KERNEL_2)<br/>gpio_init (APPLICATION)"]
    MAIN(["main 主循环<br/>事件驱动休眠/唤醒<br/>10 分钟无操作自动休眠"])

    CAN["CAN 收发线程<br/>priority 11 · 栈 2048<br/>消息收发 + 协议分发 + OTA 状态机"]
    HEART["CAN 心跳线程<br/>priority 11 · 栈 1024<br/>400ms 心跳 + 手柄状态上报 0x1E3"]

    LORA["LoRa 处理线程<br/>priority 12 · 栈 1024<br/>UART async+DMA + 统一帧解析"]
    LHEART["LoRa 心跳线程<br/>priority 12 · 栈 1024<br/>仅 LORA_TYPE 运行<br/>周期遥测 + ACK 检测"]

    ADC["ADC 采集线程<br/>priority 7 · 栈 1024<br/>X/Y 角度 500ms · 电压 5s"]
    CFG["LoRa 配参工作队列<br/>priority 8 · 栈 2048<br/>CAN 远程配参异步执行"]

    SYS --> MAIN
    SYS --> CAN
    SYS --> HEART
    SYS --> LORA
    SYS --> LHEART
    SYS --> ADC
    SYS --> CFG

    GP["global_params_t<br/>全局状态共享"]
    CAN -.读写.-> GP
    LORA -.读写.-> GP
    ADC -.读写.-> GP
```

### CAN / LoRa 通信链路

双通道冗余链路，默认 CAN，可通过 link_switch 按键（PA10）手动切换。切换时关闭对方电源并重新初始化。

```mermaid
flowchart LR
    subgraph 手柄["mod_handler"]
        CTRL["connect_type<br/>CAN_TYPE / LORA_TYPE"]
    end

    PLATFORM["平台 / 上位机"]
    GW["USR-LG210-L 网关"]
    SCANNER["激光扫描仪"]

    CTRL <-. "CAN 总线 250Kbps<br/>0x1E3 状态 / 0x763 心跳<br/>0x263·0x363·0x463 扫描仪<br/>0x105·0x106 LoRa 配参" .-> PLATFORM
    CTRL <-. "LoRa 透传<br/>统一帧 [0xAA55][NID][Len][Data][CRC16]" .-> GW
    GW --- SCANNER

    PLATFORM -. "扫描仪数据" .-> SCANNER
```

### OTA 固件升级流程

固件通过 CAN 总线分片传输，写入外部 SPI Flash（GD25Q80），由 MCUboot 以 swap-with-scratch 模式完成安全切换。

```mermaid
sequenceDiagram
    participant P as 平台
    participant M as mod_handler
    participant F as 外部 SPI Flash (GD25Q80)
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

电源键触发或 10 分钟无操作自动休眠，关闭所有外设电源（CAN / LoRa / 显示 / 手柄）；唤醒时重新上电并刷新显示。

```mermaid
stateDiagram-v2
    [*] --> 运行
    运行 --> 休眠: 电源键按下 / 10 分钟无操作
    休眠 --> 运行: 电源键唤醒

    note right of 休眠
        system_sleep():
        · 关闭 CAN/LoRa/显示/手柄电源
        · lora_deinit() 停止 DMA
        · sleeping = true
    end note

    note right of 运行
        system_wake():
        · 重新上电所有外设
        · LoRa 模式下 lora_init()
        · 等待 200ms 后 reinit 显示
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
    FTP -.下载.-> HOST
    HTTP -.下载.-> HOST
    MB <-.轮询.-> HOST
    NET -.推送.-> HOST
```

## ZephyrLink USB 复合设备

CMSIS-DAP 调试探针的 USB 设备拓扑：

```mermaid
flowchart TD
    USB["Zephyr 新版 USB 栈<br/>CONFIG_USB_DEVICE_STACK_NEXT"]
    USB --> DAP["CMSIS-DAP v2<br/>USB Bulk<br/>兼容 Keil/OpenOCD/pyOCD"]
    USB --> CDC["CDC ACM<br/>虚拟串口透传"]
    USB --> MSC["MSC<br/>拖放烧写 (可选)"]

    DAP --> SWD["SWD 调试接口<br/>SWCLK/SWDIO/nRESET"]
    CDC --> UART["USART2 桥接<br/>目标 UART 透传"]
    MSC --> FLASH["目标 Flash<br/>.hex/.bin 自动烧写"]
```

!!! tip "深入阅读"
    各应用的完整协议、引脚、Shell 命令详见 [应用模块](applications/mod-handler.md)。
