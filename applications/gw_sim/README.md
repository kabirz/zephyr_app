# gw_sim

gateway 网络代码的 native_sim 移植版。在 Linux 上编译运行,用于不依赖硬件
验证 UDP 双端口通信、配置命令、固件升级协议。

设计文档: `../../docs/superpowers/specs/2026-07-30-gw-sim-design.md`

## 构建

```shell
source ~/code/venv/zephyr/bin/activate
west build -b native_sim
```

## 运行(需两个终端,均需 sudo)

eth_native_tap 需要 CAP_NET_ADMIN,且 `zeth` 接口由 zephyr.exe 启动时自动创建
(经 `/dev/net/tun` 的 TUNSETIFF,接口名由 `CONFIG_ETH_NATIVE_TAP_DRV_NAME` 控制,默认 `zeth`)。

**终端 A** — 启动模拟器(会创建 zeth):

```shell
sudo build/zephyr/zephyr.exe
```

**终端 B** — 为 zeth 配主机 IP(carrier up 后设备侧打印 `net link up`):

```shell
./scripts/setup_tap.sh
```

设备 IP 默认 `192.168.1.100`,主机侧 zeth 为 `192.168.1.1`。

## 测试

用 socat 发包。配置端口 9200(命令),数据端口 9090(数据帧):

```shell
# GET_VERSION → 收到 [0x06]0.1.0-dev
printf '\006' | socat - UDP-DATAGRAM:192.168.1.100:9200

# GET_CONFIG → 11 字节配置
printf '\005' | socat - UDP-DATAGRAM:192.168.1.100:9200

# 广播配置(上位机未知设备 IP)
printf '\006' | socat - UDP-DATAGRAM:255.255.255.255:9200,broadcast

# 数据帧 [0x263][payload] → 终端 A 打印 rf24 stub send
printf '\002\143ABCDE' | socat - UDP-DATAGRAM:192.168.1.100:9090

# 固件升级序列(终端 A 打印字节数累加 + 重启)
printf '\020'                       | socat - UDP-DATAGRAM:192.168.1.100:9200  # FW_START
printf '\0211234567890'             | socat - UDP-DATAGRAM:192.168.1.100:9200  # FW_DATA +10B
printf '\021abcdefgh'               | socat - UDP-DATAGRAM:192.168.1.100:9200  # FW_DATA +8B
printf '\022'                       | socat - UDP-DATAGRAM:192.168.1.100:9200  # FW_END
```

## 与 gateway 的差异

- **nRF24**: stub (`src/rf24_stub.c`),`gw_rf24_send` 只打印日志不转发射频
- **固件升级**: flash 写入改 stub,仅累计字节数验证协议分包,不写 slot1
- **配置**: 内存默认值,无 settings 持久化(改 IP/RF24 重启后恢复默认)
- **网络**: eth_native_tap (`zeth`) 替代 W5500;链路就绪用 `NET_EVENT_IF_UP` 事件替代固定延时
