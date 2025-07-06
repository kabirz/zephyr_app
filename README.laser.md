# laser

## source

```shell
west init -m <this_git_url> app --mf zephyr4_1.yml
west config manifest.project-filter +canopennode
cd app
west update
```


## build

```shell
west build -b laser_stm32f103ret7 apps/applications/laser_ctrl --sysbuild
```


## generate images

```shell
apps/tools/export_images.py
```

