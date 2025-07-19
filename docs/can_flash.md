# upgrade firmware with canbus

## build firmware

```shell
west build -b f407vet6/stm32f407xx/monv apps/applications/canopen_demo --sysbuild -d canout
```

## flash

first flash
```shell
# mcubot
west flash --domain mcuboot -d canout -r probe-rs

# app
cargo-flash --chip STM32F407VETx --binary-format hex --path canout/canopen_demo/zephyr/zephyr.signed.hex
```

## upgrade

### canbus config

```shell
sudo ip link set can0 type can bitrate 250000
sudo ip link set up can0
```

canopen
```shell
   cat << EOF > ~/.canrc
   [default]
   interface = socketcan
   channel = can0
   bitrate = 250000
   EOF

```

must set sdo-timeout, the default value is 1s, but erase flash need 2s, so we can set the value > 2s,
timeout is for reboot, upgrade need about 30s

```shell
west flash  -d canout/canopen_demo -r canopen --sdo-timeout 500 --timeout 120
```
log output
```log
-- west flash: rebuilding
ninja: no work to do.
-- west flash: using runner canopen
-- runners.canopen: Using Node ID 1, program number 1
-- runners.canopen: Waiting for flash status ok
-- runners.canopen: Program software identification: 0xada237d5
-- runners.canopen: Entering pre-operational mode
-- runners.canopen: Stopping program
-- runners.canopen: Clearing program
-- runners.canopen: Waiting for flash status ok
-- runners.canopen: Downloading program: /home/zhp/code/app/canout/canopen_demo/zephyr/zephyr.signed.bin
100% |################################| 112456/112456B
-- runners.canopen: Waiting for flash status ok
-- runners.canopen: Program software identification: 0xada237d5
-- runners.canopen: Starting program
-- runners.canopen: Waiting for boot-up message...
-- runners.canopen: Program software identification: 0xada237d5
-- runners.canopen: Entering pre-operational mode
-- runners.canopen: Confirming program
```
