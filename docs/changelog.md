# 变更日志

本项目遵循 [Conventional Commits](https://www.conventionalcommits.org/) 规范。版本标签（`v*` / `v*.mod` / `v*.laser`）通过 GitHub Release 发布对应固件。

## 应用当前版本

| 应用 | 版本 | 最新标签 | 日期 |
|------|------|----------|------|
| mod_handler | `0.1.4-release` | `v0.1.4.mod` | 2026-06-03 |
| data_collect | `2.1.2-release` | `v2.2.1` | 2025-11-16 |
| laser_ctrl | `0.1.9-release` | `v2.2.1.laser` | 2026-02-04 |
| ZephyrLink (dapLink) | 预览版 | — | — |

## [未发布]

### dapLink / ZephyrLink

- **feat**: WebUSB 支持，浏览器端 CMSIS-DAP 调试
- **feat**: 新增 dapLink 应用（基于 Zephyr 的 CMSIS-DAP v2 固件）
- **feat**: 新增 pyocd / probe-rs 烧录支持

### 全局

- **refactor**: 优化 LoRa 配参请求结构，替换 `lora_configure`
- **feat**(config): 连接类型设置持久化存储
- **fix**: mod_handler 线程安全、显示 bug、休眠竞态修复
- **ci**: 更新 CI host 配置

## [v0.1.4.mod] — 2026-06-03

### mod_handler 0.1.4

激光测距手持控制器，CAN/LoRa 双链路版本。

- **feat**: ADC 动态休眠、CAN 协议调整、CI 更新
- **fix**: 线程安全、显示 bug、休眠竞态修复
- **fix**: 按键状态处理（CAN / LoRA 模块）
- **fix**: `k_event_set` 误清除其他事件位
- **fix**: LoRa 与电源管理关键 bug
- **fix**: LoRa AT 模式经 CAN 配置时死锁（改用专用工作队列）
- **feat**: 连接类型设置持久化
- **feat**: 显示布局编译期开关
- **feat**: ADC XY 采样 0.1° 精度 + 修剪平均
- **feat**: 电池状态显示
- **refactor**: 扫描仪数据改用结构体 + 位域有效性标志 + 大端序辅助函数
- **feat**: LoRa 扫描仪数据合并为单帧（20 字节）
- **feat**: 休眠与时间管理、Shell sleep 命令
- **fix**: LoRa 帧解析改用 ring buffer
- **feat**: 半双工节流（`lora_data_sem` 500ms 超时）
- **feat**: CAN 测试 / 电源 Shell 命令

## [v2.2.1.laser] — 2026-02-04

### laser_ctrl 0.1.9

- **feat**: laser_ctrl v0.1.9
- **feat**: west packages 支持
- **feat**: west patch 支持
- **feat**: mod_handler 模块初始化
- **feat**: tools CAN 升级 CLI（基于 pydantic-settings）
- **fix**: 移除 on flag 与 rs485 dts
- **feat**: manifest 更新

## [v2.2.1] — 2025-11-16

### data_collect 主线

- **feat**: Zephyr 升级至 v4.3
- **feat**: PSRAM 启用（lvgl 显示）
- **feat**: SPI master/slave demo
- **feat**: f407 USB snippets
- **feat**: monv 板支持 sdcard 与 esp8266
- **feat**: flash 文件系统驱动
- **feat**: libs `sys_read16` 工具
- **feat**: laser native_sim_64 仿真
- **ci**: Actions 触发条件、email 通知、apply patches

## 早期版本

| 标签 | 日期 | 说明 |
|------|------|------|
| `v2.2.0` | 2025-11-15 | data_collect 增量迭代 |
| `v2.1.5` ~ `v2.1.9` | 2025-11-14 | laser / data_collect 增量迭代 |
| `v2.1.4` | 2025-11-14 | 协议与外设完善 |
| `v2.1.0` ~ `v2.1.2` | 2024-12 | 功能完善阶段 |
| `v2.0.0` ~ `v2.0.1` | 2024-11 | 基线版本 |

!!! info "查看完整历史"
    完整提交历史见 [GitHub Commits](https://github.com/kabirz/zephyr_app/commits/main)，版本发布见 [Releases](https://github.com/kabirz/zephyr_app/releases)。
