# 统一链路发送逻辑：合并手柄状态发送 + 统一心跳

- 日期：2026-07-13
- 状态：已批准（待实现）
- 涉及模块：mod_handler（common / can / rf24 / gpio / adc）

## 背景与动机

mod_handler 通过 CAN 或 2.4G (nRF24L01+) 两种互斥链路与激光设备通信，运行时由
`connect_type`（`CAN_TYPE` / `RF24_TYPE`）决定走哪条链路。

当前存在两处"按模式分派发送"的重复与不对称：

1. **手柄状态发送**：`mod_can_send_handler_state`（can.c）与 `rf24_send_telemetry`
   （rf24.c）是同一件事的两个实现，调用点 gpio.c / adc.c 各写一遍
   `if (connect_type == CAN_TYPE) ... else ...`，违反 DRY。
2. **心跳不对称**：仅 CAN 模式有 `can_heart_thread` 发 0x763 保活帧；RF24 模式无
   周期性上报。链路保活语义不统一。

## 目标

- 把手柄状态发送合并为单一入口 `send_handler_state`，按 `connect_type` 内部分派。
- 把心跳改为"周期状态上报"统一模型，CAN/RF24 对称，去掉 0x763 专用保活帧。
- 所有按模式分派的发送逻辑集中到新建的 `common.c`，main.c 保持纯主循环职责。

## 非目标

- 不改变 `connect_switch` 的链路切换机制（仍放 gpio.c）。
- 不改变事件驱动的触发时机（按键/角度变化仍即时发）。
- 不引入新链路类型（YAGNI）。
- `CAN_SLEEP_MS` / `RF24_SLEEP_MS`（adc 上报节流）保留不变。

## 现状分析

### 调用点（对称重复）
- `gpio.c:49-53`（按键事件）
- `adc.c:133-137`（角度变化）

两处均为 `if (connect_type == CAN_TYPE) mod_can_send_handler_state() else
rf24_send_telemetry()`，且都**不检查返回值**。

### 函数实现差异
| | `mod_can_send_handler_state` (can.c:188) | `rf24_send_telemetry` (rf24.c:98) |
|---|---|---|
| 返回类型 | `int` | `bool` |
| 前置守卫 | `heart_send_success`（心跳未成功 → return -1） | 无 |
| 载荷长度 | 8 字节（dlc=8） | 7 字节 |
| 帧 ID | `HANDLER_STATE` (0x1E3) | `HANDLER_STATE` (0x1E3) |
| 日志 | `params->log` 时打印 x/y/btn | 在 `rf24_data_send` 内部 |

### 心跳现状（`can_heart_thread`，can.c:137-178，仅 CAN）
- 周期发 `COBID_HEATBEAT` (0x763) 保活帧，靠对端 ACK 经 `heart_tx_callback` 置
  `heart_send_success=1`。
- 连续 3 次未 ACK → 清 `CAN_RX_EVENT`、`heart_send_success=0`（判定断连）。
- 手柄状态 0x1E3 受 `heart_send_success` 守卫。
- RF24 模式无心跳线程（靠每次发送的硬件 ACK）。

## 设计

### 新建 `src/common.c`（通用发送逻辑模块）

需 include：`<zephyr/kernel.h>`、`<zephyr/drivers/can.h>`（`struct can_frame` /
`can_bytes_to_dlc`）、`<zephyr/sys/byteorder.h>`（`sys_put_be16`）、
`<zephyr/logging/log.h>`（`LOG_INF`）、`<common.h>`、`<mod-can.h>`
（`mod_can_send` / `HANDLER_STATE`）、`<rf24.h>`（`rf24_data_send`）。

#### ① `send_handler_state`

合并 `mod_can_send_handler_state` + `rf24_send_telemetry`，去掉 `heart_send_success`
守卫，统一返回 `int`（<0 失败）：

```c
int send_handler_state(const global_params_t *params)
{
    if (params->connect_type == CAN_TYPE) {
        struct can_frame frame = { .id = HANDLER_STATE, .dlc = can_bytes_to_dlc(8) };
        sys_put_be16((uint16_t)params->x_degree, &frame.data[0]);
        sys_put_be16((uint16_t)params->y_degree, &frame.data[2]);
        frame.data[4] = params->h_button;
        frame.data[5] = frame.data[6] = frame.data[7] = 0xFF;
        if (params->log) {
            LOG_INF("x: %d, y: %d, button: %d",
                    params->x_degree, params->y_degree, params->h_button);
        }
        return mod_can_send(&frame);
    } else {
        uint8_t payload[7];
        sys_put_be16((uint16_t)params->x_degree, &payload[0]);
        sys_put_be16((uint16_t)params->y_degree, &payload[2]);
        payload[4] = params->h_button;
        payload[5] = payload[6] = 0xFF;
        return rf24_data_send(HANDLER_STATE, payload, sizeof(payload)) ? 0 : -EIO;
    }
}
```

既服务事件驱动调用点（gpio/adc），也服务心跳（周期）。

#### ② `heart_thread`（统一心跳，替代 `can_heart_thread`）

```c
static void heart_thread(void)
{
    while (true) {
        k_event_wait(&global_params.event, CAN_EVENT | RF24_EVENT, false, K_FOREVER);
        if (global_params.sleeping) {
            k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
            continue;
        }
        send_handler_state(&global_params);
        k_sleep(K_MSEC(global_params.report_period));
    }
}
K_THREAD_DEFINE(thread_heart, 1024, heart_thread, NULL, NULL, NULL, 11, 0, 0);
```

- 等 `CAN_EVENT | RF24_EVENT` 任一置位（`connect_switch` 在对应模式已置位）。
- 周期复用 `send_handler_state`，按当前 `connect_type` 自动分派。
- 去掉 0x763 保活帧、`heart_send_success`、`heart_tx_callback`、断连 3 次停止逻辑。

### `common.h` 变更
- 新增声明：`int send_handler_state(const global_params_t *params);`
- 新增宏 `#define REPORT_PERIOD_MS 800`（替代 `mod-can.h` 的 `CAN_HEART_TIME`，通道无关）。
- 字段 `can_heart_time` → `report_period`（通用化，CAN/RF24 共用周期）。
- 初始化点 `gpio.c:216`：`global_params.can_heart_time = CAN_HEART_TIME;`
  → `global_params.report_period = REPORT_PERIOD_MS;`

### `CMakeLists.txt`
- source list 加入 `src/common.c`。

## 文件变更清单

**新增**
- `src/common.c`：`send_handler_state` + `heart_thread` + `thread_heart`
- `CMakeLists.txt`：source list 加 `common.c`

**修改**
- `common.h`：+ `send_handler_state` 声明；`can_heart_time` → `report_period`
- `gpio.c`（L49-53）：`if/else` → `send_handler_state(&global_params);`
- `adc.c`（L133-137）：`if/else` → `send_handler_state(&global_params);`
- 所有初始化 `can_heart_time` 的点 → `report_period`

**删除**
- `can.c`：`mod_can_send_handler_state`、`can_heart_thread`、`heart_tx_callback`、
  `heart_send_success`、`thread_can_heart`
- `rf24.c`：`rf24_send_telemetry`
- `mod-can.h`：`mod_can_send_handler_state` 声明、`COBID_HEATBEAT`、`CAN_HEART_TIME`
- `rf24.h`：`rf24_send_telemetry` 声明

**不变**：`src/main.c`（纯主循环：休眠/唤醒/超时）。

## 决策记录

1. **完全合并 vs 包装分派**：选完全合并。真正单一函数体，彻底消除重复，调用点变单行。
2. **归属 common.c（新建）**：`send_handler_state` 与 `heart_thread` 都放新建 `common.c`，
   集中按模式分派逻辑。`main.c` 保持纯主循环，职责不混合。
3. **周期状态上报 vs 保活帧分派**：选周期状态上报。去掉 0x763 保活帧与
   `heart_send_success`，心跳=周期发手柄状态，CAN/RF24 完全对称。
4. **去掉断连检测**：原 3 次失败停止逻辑移除。周期重发本身即容错，失败仅 log（YAGNI）。
5. **心跳激活事件**：等 `CAN_EVENT | RF24_EVENT` 任一。
6. **不再等首帧 CAN 数据确认**：原 `can_heart_thread` 用
   `k_event_wait_all(CAN_EVENT | CAN_RX_EVENT)`，要求收到首帧 CAN 数据
  （`CAN_RX_EVENT`，由 `mod_can_process_thread` 收首帧时 post）才开始心跳。新设计
   去掉 `CAN_RX_EVENT` 依赖，`CAN_EVENT` 置位即周期上报——这是去掉断连检测的逻辑
   延伸（主动周期探测，而非被动等待链路确认）。

## 风险与验证

- **行为变化**：CAN 模式不再发 0x763 保活帧；若平台/网关依赖 0x763 判活，需对端适配。
  验证：实现前与平台方确认 0x763 是否被依赖。
- **断连无主动停止**：CAN 断连后仍周期重发 0x1E3（`can_send` 失败返回 <0，仅 log）。
  可接受——重连后自动恢复上报。
- **RF24 模式新增并发**：原 RF24 无心跳线程，`rf24_tx_mutex` 只序列化 adc/gpio 事件
  TX。新增 `heart_thread`（优先级 11）会周期与 adc(pri 7) / gpio 工作队列竞争同一
  mutex。功能安全（mutex 保护），但高优先级心跳可能短暂延迟事件驱动 TX。验证项需
  覆盖 RF24 模式下事件上报与周期心跳的交错。
- **验证方式**：实现后 `west build -b nrf24_f103rct6 applications/mod_handler --sysbuild
  --pristine` 零警告；烧录后实测 CAN/RF24 两模式的事件驱动上报 + 周期心跳。
```
