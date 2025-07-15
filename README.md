# Zephyr enviroment setup
[help](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)

sdk version: zephyr-sdk-0.16.8

# zephyr application

## zephyr source code

```shell
west init -m <this_git_url> app
cd app
west update
```

## build data_collect

support f407vet6/stm32f407xx/daq and apollo_h743ii

## applay all patches
```shell
# build
# f407vet6/stm32f407xx/daq
west build -b f407vet6/stm32f407xx/daq apps/applications/data_collect --sysbuild

# apollo_h743ii
west build -b apollo_h743ii apps/applications/data_collect --sysbuild
```

## generate app

```shell
python apps/tools/export_images.py build # build is output folder name
```

## image

* build/app.bin: all image(mcuboot+zephyr).
* build/data_collect/zephyr/zephyr.signed.bin: signed image for upgrade.


## [smp-tool](https://github.com/Gessler-GmbH/smp-rs/tree/main/smp-tool)

### shell
```shell
  smp-tool -t udp -d 192.168.12.101 shell interactive
```
### flash
debug tool
```shell
cargo-flash --chip STM32F407VETx/STM32H743IITx --binary-format bin --base-address 0x8000000 --path build/app.bin
```
smp-tool:
```shell
  smp-tool -t udp -d 192.168.12.101 app flash -u build/data_collect/zephyr/zephyr.signed.bin
```
python tool:
```shell
# mcuboot flash
./tools/smp_upload.py -a 192.168.12.101 --mcuboot build/data_collect/zephyr/zephyr.signed.bin
# app
./tools/smp_upload.py -a 192.168.12.101 build/data_collect/zephyr/zephyr.signed.bin
```

