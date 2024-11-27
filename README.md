# zephyr application

## zephyr source code

```shell
west init zephyr
cd zephyr
west update
export ZEPHYR_BASE=`pwd`/zephyr
```

## build data_collect for daq

```shell
west build -b daq_f407vet6 applications/data_collect -DBOARD_ROOT=`pwd` --sysbuild
```

## generate app

```shell
./tools/gen_app.sh
```

## image

* build/app.bin: all image for daq board
* build/data_collect/zephyr/zephyr.signed.bin: signed image for daq board

