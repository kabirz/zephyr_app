# 构建与烧录

所有应用均使用 `west build ... --sysbuild` 构建，sysbuild 会同时编译 **MCUboot bootloader** 与应用镜像。

## 构建镜像

=== "data_collect"

    支持板：`f407vet6/stm32f407xx/daq`、`apollo_h743ii`

    ```bash
    # f407vet6/stm32f407xx/daq
    west build -b f407vet6/stm32f407xx/daq apps/applications/data_collect --sysbuild

    # apollo_h743ii
    west build -b apollo_h743ii apps/applications/data_collect --sysbuild
    ```

=== "mod_handler"

    ```bash
    west build -b nrf24_f103rct6 apps/applications/mod_handler --sysbuild

    # 带 shell 与 imgmgr 调试工具
    west build -b nrf24_f103rct6 apps/applications/mod_handler --sysbuild \
        -Dmod_handler_SNIPPET=imgmgr-shell
    ```

=== "laser_ctrl"

    ```bash
    west build -b laser_f103ret7 apps/applications/laser_ctrl --sysbuild

    # 启用 mcumgr shell 下载
    west build -b laser_f103ret7 apps/applications/laser_ctrl --sysbuild \
        -Dlaser_ctrl_SNIPPET=imgmgr-shell
    ```

=== "ZephyrLink"

    ```bash
    west build -b stm32f103_mini apps/applications/dapLink
    ```

=== "canopen_demo"

    ```bash
    west build -b f407vet6/stm32f407xx/monv apps/applications/canopen_demo --sysbuild -d canout
    ```

## 导出镜像

```bash
python apps/tools/export_images.py build   # build 为构建输出目录名
```

构建产物：

| 文件 | 说明 |
|------|------|
| `build/app.bin` | 完整镜像（MCUboot + Zephyr 应用） |
| `build/<app>/zephyr/zephyr.signed.bin` | 签名镜像，用于 OTA 升级 |

## 烧录

sysbuild 产物包含 mcuboot + app，在 `build` 根目录执行 `west flash` 会按 `flash_order` 依次烧录全部镜像。

=== "pyocd（默认）"

    ```bash
    west flash
    ```

=== "openocd"

    ```bash
    west flash --runner openocd
    ```

=== "probe-rs / cargo-flash"

    ```bash
    # 仅烧录应用镜像
    cargo-flash --chip STM32F407VETx/STM32H743IITx \
        --binary-format bin --base-address 0x8000000 --path build/app.bin
    ```

## OTA 升级（已运行设备）

设备运行后，可通过网络（SMP）或 CAN 总线远程升级，无需连接调试器。

=== "smp-tool"

    [smp-tool](https://github.com/Gessler-GmbH/smp-rs/tree/main/smp-tool) 交互式 Shell：

    ```bash
    # 进入 shell
    smp-tool -t udp -d 192.168.12.101 shell interactive

    # 远程刷写
    smp-tool -t udp -d 192.168.12.101 app flash \
        -u build/data_collect/zephyr/zephyr.signed.bin
    ```

=== "Python 工具"

    ```bash
    # mcuboot 镜像
    ./tools/smp_upload.py -a 192.168.12.101 --mcuboot build/data_collect/zephyr/zephyr.signed.bin
    # 应用镜像
    ./tools/smp_upload.py -a 192.168.12.101 build/data_collect/zephyr/zephyr.signed.bin
    ```

=== "CAN 总线"

    详见 [CAN 总线升级](can_flash.md)。

!!! note "调试器直连刷写"
    首次烧录或设备无法联网时，使用 `cargo-flash` / `pyocd` / `probe-rs` 通过 SWD 直连刷写完整镜像 `build/app.bin`。
