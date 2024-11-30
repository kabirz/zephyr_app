#!/bin/bash


git -C $ZEPHYR_BASE/../bootloader/mcuboot am $PWD/patches/0001-mcuboot-support-udp-upgrade.patch

