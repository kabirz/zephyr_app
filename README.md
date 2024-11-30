# Zephyr enviroment setup
[help](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)

sdk version: zephyr-sdk-0.16.8

# zephyr application

## zephyr source code

```shell
west init zephyr
cd zephyr
# work in 7271000fe56
git reset --hard 7271000fe56
west update
export ZEPHYR_BASE=`pwd`/zephyr
```

## build data_collect for daq

support daq_f407vet6  and apollo_h743ii

```shell
# patch for mcuboot
./tools/mcu_patch.sh
# build
west build -b daq_f407vet6/apollo_h743ii applications/data_collect -DBOARD_ROOT=`pwd` --sysbuild
```

## generate app and flash

```shell
./tools/flash_f407.sh
# or
./tools/flash_h743.sh
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

