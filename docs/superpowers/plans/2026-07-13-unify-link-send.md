# 统一链路发送逻辑 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `mod_can_send_handler_state` + `rf24_send_telemetry` 合并为 `common.c` 的单一 `send_handler_state`（按 `connect_type` 分派），并把心跳统一为周期状态上报线程 `heart_thread`（复用 `send_handler_state`）。

**Architecture:** 新建 `src/common.c` 集中所有"按连接模式分派的发送"：`send_handler_state`（事件驱动，被 gpio/adc 调用）+ `heart_thread`（周期）。删除 `can.c` 的 CAN 专用心跳/0x763 保活帧/`heart_send_success` 与 `rf24.c` 的 `rf24_send_telemetry`。`main.c` 不变。

**Tech Stack:** Zephyr RTOS, C11, STM32F103RCT6, nRF24L01+, MCUBoot sysbuild

**Spec:** `docs/superpowers/specs/2026-07-13-unify-link-send-design.md`

**验证范式（嵌入式固件，无单测框架）:** 每个 task 验证 = 编译通过 + grep 核查符号无残留；最终 task 用 pristine sysbuild 确认零警告 + 烧录实测 CAN/RF24 两模式。

**编译命令模板（增量，快）:**
```bash
source ~/code/venv/zephyr/bin/activate && cd /home/zed/code/rtos_app/apps && west build -b nrf24_f103rct6 applications/mod_handler --sysbuild
```

**所有路径相对:** `/home/zed/code/rtos_app/apps/applications/mod_handler/`

---

## 文件结构

| 文件 | 责任 | 本计划改动 |
|------|------|-----------|
| `include/common.h` | 全局状态类型 + `global_params` extern | 字段重命名 + 加声明/宏 |
| `src/common.c` | **新建**：按模式分派的发送逻辑 | 创建（`send_handler_state` + `heart_thread`）|
| `src/can.c` | CAN 收发 + 解析 | 删心跳/保活/手柄状态发送 |
| `src/rf24.c` | 2.4G 收发 | 删 `rf24_send_telemetry` |
| `include/mod-can.h` | CAN 协议定义 | 删 `COBID_HEATBEAT`/`CAN_HEART_TIME`/声明 |
| `include/rf24.h` | RF24 接口 | 删 `rf24_send_telemetry` 声明 |
| `src/gpio.c` / `src/adc.c` | 调用点 | if/else → `send_handler_state` |
| `CMakeLists.txt` | 构建 | source list 加 `common.c` |
| `src/main.c` | 主循环 | **不变** |

---

## Task 1: 字段重命名 `can_heart_time` → `report_period` + 新宏

**Files:**
- Modify: `include/common.h`（字段重命名 + 加 `REPORT_PERIOD_MS` 宏）
- Modify: `src/gpio.c:216`（初始化点）
- Modify: `src/can.c:172`（`can_heart_thread` 内引用）
- Modify: `include/mod-can.h:44`（删 `CAN_HEART_TIME` 宏）

- [ ] **Step 1: common.h 字段重命名 + 加宏**

`include/common.h` 中把字段 `can_heart_time` 改为 `report_period`，并在 `CAN_TYPE`/`RF24_TYPE` 宏附近加：
```c
#define REPORT_PERIOD_MS 800   /* 周期状态上报间隔，CAN/RF24 共用 */
```

- [ ] **Step 2: 修正所有 `can_heart_time` 引用**

`src/gpio.c:216`：
```c
global_params.can_heart_time = CAN_HEART_TIME;
```
→
```c
global_params.report_period = REPORT_PERIOD_MS;
```

`src/can.c:172-173`：
```c
if (diff < global_params.can_heart_time) {
    k_sleep(K_MSEC(global_params.can_heart_time - diff));
```
→
```c
if (diff < global_params.report_period) {
    k_sleep(K_MSEC(global_params.report_period - diff));
```

- [ ] **Step 3: 删 `mod-can.h` 的 `CAN_HEART_TIME` 宏**

`include/mod-can.h:44` 删除：
```c
#define CAN_HEART_TIME 800
```

- [ ] **Step 4: grep 核查无残留**

Run: `grep -rn "can_heart_time\|CAN_HEART_TIME" src/ include/`
Expected: 无输出

- [ ] **Step 5: 编译验证（增量）**

Run: 编译命令模板
Expected: 编译成功（Kconfig/字段重命名已生效）

- [ ] **Step 6: Commit**
```bash
git add include/common.h include/mod-can.h src/gpio.c src/can.c
git commit -m "refactor(mod_handler): rename can_heart_time to report_period"
```

---

## Task 2: 新建 `common.c` 含 `send_handler_state`

**Files:**
- Create: `src/common.c`
- Modify: `include/common.h`（加 `send_handler_state` 声明）
- Modify: `CMakeLists.txt`（source list 加 `common.c`）

- [ ] **Step 1: 创建 `src/common.c`**

```c
/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 通用链路发送：按 connect_type 分派 CAN / 2.4G (nRF24L01+)。
 * send_handler_state 由事件驱动调用点 (gpio/adc) 与周期心跳线程复用。
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <common.h>
#include <mod-can.h>
#include <rf24.h>

LOG_MODULE_REGISTER(common_link, LOG_LEVEL_INF);

/* 手柄状态帧 (HANDLER_STATE=0x1E3)：[x 2B BE][y 2B BE][btn][0xFF...]
 * CAN 用 8 字节 dlc，RF24 用 7 字节 payload（无 CAN 的末字节 0xFF）。
 */
int send_handler_state(const global_params_t *params)
{
	if (params->connect_type == CAN_TYPE) {
		struct can_frame frame = {
			.id = HANDLER_STATE,
			.dlc = can_bytes_to_dlc(8),
		};
		sys_put_be16((uint16_t)params->x_degree, &frame.data[0]);
		sys_put_be16((uint16_t)params->y_degree, &frame.data[2]);
		frame.data[4] = params->h_button;
		frame.data[5] = 0xFF;
		frame.data[6] = 0xFF;
		frame.data[7] = 0xFF;
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
		payload[5] = 0xFF;
		payload[6] = 0xFF;
		return rf24_data_send(HANDLER_STATE, payload, sizeof(payload)) ? 0 : -EIO;
	}
}
```

- [ ] **Step 2: common.h 加声明**

`include/common.h`（在 `extern global_params_t global_params;` 附近）加：
```c
/* 按 connect_type 分派发送手柄状态帧 (CAN 0x1E3 / RF24 遥测)。返回 0 成功，<0 失败。*/
int send_handler_state(const global_params_t *params);
```

- [ ] **Step 3: CMakeLists.txt 加 common.c**

`CMakeLists.txt` 的 source list 加一行 `src/common.c`（参照现有 `src/can.c` 等的格式）。

- [ ] **Step 4: 编译验证（增量）**

Run: 编译命令模板
Expected: 编译成功。`send_handler_state` 是全局函数未被调用，**不**产生 `-Wunused`（仅 static 才警告）。

- [ ] **Step 5: Commit**
```bash
git add src/common.c include/common.h CMakeLists.txt
git commit -m "feat(mod_handler): add common.c send_handler_state (CAN/RF24 dispatch)"
```

---

## Task 3: 迁移调用点到 `send_handler_state`

**Files:**
- Modify: `src/gpio.c:49-53`
- Modify: `src/adc.c:133-137`

- [ ] **Step 1: gpio.c 调用点**

`src/gpio.c:49-53`：
```c
if (global_params.connect_type == CAN_TYPE) {
    mod_can_send_handler_state(&global_params);
} else {
    rf24_send_telemetry(&global_params);
}
```
→
```c
send_handler_state(&global_params);
```

- [ ] **Step 2: adc.c 调用点**

`src/adc.c:133-137`：
```c
if (global_params.connect_type == CAN_TYPE) {
    mod_can_send_handler_state(&global_params);
} else {
    rf24_send_telemetry(&global_params);
}
```
→
```c
send_handler_state(&global_params);
```

- [ ] **Step 3: 编译验证（增量）**

Run: 编译命令模板
Expected: 编译成功。旧 `mod_can_send_handler_state`/`rf24_send_telemetry` 仍在但未被调用（全局函数，不警告）。

- [ ] **Step 4: Commit**
```bash
git add src/gpio.c src/adc.c
git commit -m "refactor(mod_handler): route handler-state calls via send_handler_state"
```

---

## Task 4: 心跳迁移到 `common.c` + 删除旧发送/心跳代码

**Files:**
- Modify: `src/common.c`（追加 `heart_thread` + `thread_heart`）
- Modify: `src/can.c`（删 `can_heart_thread`/`heart_tx_callback`/`heart_send_success`/`thread_can_heart`/`mod_can_send_handler_state`）
- Modify: `include/mod-can.h`（删 `mod_can_send_handler_state` 声明、`COBID_HEATBEAT`）
- Modify: `src/rf24.c`（删 `rf24_send_telemetry`）
- Modify: `include/rf24.h`（删 `rf24_send_telemetry` 声明）

- [ ] **Step 1: common.c 追加统一心跳线程**

`src/common.c` 末尾追加：
```c
/* 统一心跳：周期上报手柄状态，按 connect_type 自动分派 CAN/RF24。
 * 等 CAN_EVENT 或 RF24_EVENT 任一置位（connect_switch 切换时置位）。
 */
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

- [ ] **Step 2: can.c 删除旧心跳 + 手柄状态发送**

`src/can.c` 删除以下符号（连同其 `K_THREAD_DEFINE(thread_can_heart, ...)`）：
- `static atomic_t heart_send_success`（约 L20）
- `static void heart_tx_callback(...)`（约 L39-44）
- `static void can_heart_thread(void)`（约 L137-176）+ 其 `K_THREAD_DEFINE(thread_can_heart, ...)`（L178）
- `int mod_can_send_handler_state(const global_params_t *params)`（约 L188-212）

**注意：** 删除前确认 `atomic.h` 等 include 若仅服务上述符号也可清理（保留以防其他原子用途——grep `atomic_` 确认）。

- [ ] **Step 3: mod-can.h 删声明 + COBID_HEATBEAT**

`include/mod-can.h` 删除：
- `COBID_HEATBEAT = 0x763,`（枚举项，约 L12）
- `int mod_can_send_handler_state(const global_params_t *params);`（约 L64）

- [ ] **Step 4: rf24.c 删 `rf24_send_telemetry`**

`src/rf24.c` 删除 `bool rf24_send_telemetry(const global_params_t *params)` 整个函数（约 L98-109）。

- [ ] **Step 5: rf24.h 删声明**

`include/rf24.h` 删除 `bool rf24_send_telemetry(const global_params_t *params);`（约 L55）。

- [ ] **Step 6: grep 核查所有删除符号无残留**

Run:
```bash
grep -rn "mod_can_send_handler_state\|rf24_send_telemetry\|can_heart_thread\|thread_can_heart\|heart_send_success\|heart_tx_callback\|COBID_HEATBEAT" src/ include/
```
Expected: 无输出

- [ ] **Step 7: 编译验证（增量）**

Run: 编译命令模板
Expected: 编译成功，无 `-Wunused-function`（旧函数已删，新函数都被调用）。

- [ ] **Step 8: Commit**
```bash
git add src/common.c src/can.c src/rf24.c include/mod-can.h include/rf24.h
git commit -m "refactor(mod_handler): unify heartbeat into common.c, drop CAN-only keepalive"
```

---

## Task 5: 全面验证（pristine sysbuild + 符号核查 + 烧录测试）

- [ ] **Step 1: pristine sysbuild 编译零警告**

Run:
```bash
source ~/code/venv/zephyr/bin/activate && cd /home/zed/code/rtos_app/apps && west build -b nrf24_f103rct6 applications/mod_handler --sysbuild --pristine > /tmp/verify.log 2>&1
echo $?
grep -ciE "\.[ch]:.*warning:" /tmp/verify.log
```
Expected: exit 0；warning 计数 0。

- [ ] **Step 2: 全量符号核查（删除/重命名均无残留）**

Run:
```bash
grep -rn "mod_can_send_handler_state\|rf24_send_telemetry\|can_heart_thread\|heart_send_success\|heart_tx_callback\|COBID_HEATBEAT\|CAN_HEART_TIME\|can_heart_time" src/ include/
```
Expected: 无输出。

- [ ] **Step 3: 烧录功能测试（手动）**

烧录后实测两种模式：
- **CAN 模式**：操纵杆/按键变化即时发 0x1E3；周期（~800ms）发 0x1E3 心跳上报；不再发 0x763。
- **RF24 模式**：操纵杆/按键变化即时发遥测；周期发遥测保活；link_switch 切换正常。
- **休眠/唤醒**：心跳随休眠停止、唤醒恢复。

- [ ] **Step 4: 若有清理则 Commit**

如 Step 1-2 发现遗漏并修复：
```bash
git add -A
git commit -m "fix(mod_handler): cleanup remaining references after link-send unification"
```

---

## 风险回顾（来自 spec）

- 平台/网关若依赖 0x763 判活，需对端适配（Step 3 烧录测试时确认）。
- RF24 模式新增 `heart_thread`(pri 11) 与 adc(pri 7)/gpio 竞争 `rf24_tx_mutex`——功能安全，测试时关注事件上报与周期心跳交错。
