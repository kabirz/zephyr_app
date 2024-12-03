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

## build data_collect for daq

support daq_f407vet6  and apollo_h743ii

## applay all patches
```shell
./apps/tools/apply_patches.sh
# build
west build -b daq_f407vet6/apollo_h743ii apps/applications/data_collect --sysbuild
```

## generate app

```shell
./apps/tools/gen_app.sh
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

