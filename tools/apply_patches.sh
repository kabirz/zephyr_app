#!/bin/bash


git -C "$(west topdir)"/bootloader/mcuboot am "$(west topdir)"/apps/patches/0001-mcuboot-support-udp-upgrade.patch

