# laser

## source

```shell
west init -m <this_git_url> app --mf zephyr4_1.yml
cd app
west update
```

## apply patches
```shell
git -C ../zephyr am `pwd`/patches/0001-serail-data-bit-8-stopbit-1-parity-even.patch
```

## build

```shell
west build -b laser_f103ret7 apps/applications/laser_ctrl --sysbuild
# enable mcumgr shell download
west build -b laser_f103ret7 apps/applications/laser_ctrl --sysbuild -Dlaser_ctrl_SNIPPET=imgmgr-shell
```


## generate images

```shell
apps/tools/export_images.py
```

