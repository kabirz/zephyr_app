#!/bin/bash

cur_size=$(stat -c%s build/mcuboot/zephyr/zephyr.bin)

fill_size=$((128*1024 - cur_size))

echo $fill_size
dd if=/dev/zero bs=1 count=$fill_size | tr '\0' '\377' > tmp.bin

cp build/mcuboot/zephyr/zephyr.bin build/app.bin
cat tmp.bin >> build/app.bin

cat build/data_collect/zephyr/zephyr.signed.bin >> build/app.bin

rm tmp.bin
