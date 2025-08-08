# zephyr devicetree

# how to include file to dts

```dts
	zephyr,user {
		zhp_data = /incbin/("../VERSION");
	};
```

in c file
```c
  const uint8_t data[] = DT_PROP(DT_PATH(zephyr_user), zhp_data);
```

