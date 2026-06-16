# canopen_demo CANopen

基于 [CANopenNode](https://github.com/CANopenNode/CANopenNode) 协议栈的 CANopen 示例，演示在 Zephyr 中集成标准化的 CANopen 通信，支持对象字典非易失存储与 CAN 总线程序下载（OTA）。

!!! info "原始文档"
    本页为概要。完整说明见仓库源文件 `applications/canopen_demo/README.rst`。

## 功能概述

- **CANopen 协议栈** — 遵循 EN 50325-4 / CiA 301 国际标准
- **对象字典存储** — 非易失 Flash 存储对象字典配置
- **CANopen 程序下载** — 通过 CANopen SDO 实现 OTA 固件升级
- **LED 指示** — CANopen 红绿状态指示灯（NMT 状态）

## 模块启用

```bash
west config manifest.project-filter +canopennode
west update canopennode
```

## 构建

```bash
west build -b f407vet6/stm32f407xx/monv apps/applications/canopen_demo --sysbuild -d canout
```

## CANopen 程序下载（OTA）

支持通过 CANopen SDO 远程刷写固件，结合 MCUboot：

```bash
# 首次烧录 bootloader 与应用
west flash --domain mcuboot -d canout -r probe-rs
cargo-flash --chip STM32F407VETx --binary-format hex \
    --path canout/canopen_demo/zephyr/zephyr.signed.hex

# 配置 CAN 总线
sudo ip link set can0 type can bitrate 250000
sudo ip link set up can0

# 通过 CANopen 升级 (擦除需 2s+, 设置 sdo-timeout > 2s)
west flash -d canout/canopen_demo -r canopen --sdo-timeout 500 --timeout 120
```

!!! warning "Flash 擦除块限制"
    若 flash-controller 的 `<erase-block-size>` 超过 `0x10000`（如 `nucleo_h743zi` 的 128KB 擦除块），本示例无法运行。

## Python 通信测试

```bash
pip3 install --user canopen python-can
```

可进行 NMT 状态切换、SDO 读写、PDO 映射等操作，详见 [python-canopen](https://github.com/christiansandberg/canopen)。

## 对象字典

对象字典位于 `applications/canopen_demo/objdict/`，可使用 [libedssharp](https://github.com/robincornelius/libedssharp) EDS 编辑器修改 `objdict.xml` 并重新生成 `CO_OD.h` / `CO_OD.c`。

## 参考

- [CANopenNode](https://github.com/CANopenNode/CANopenNode)
- [CiA 301 标准](https://can-cia.org/cia-groups/technical-documents/)
- [CANopen for Python](https://github.com/christiansandberg/canopen)
