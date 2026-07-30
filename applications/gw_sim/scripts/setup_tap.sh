#!/bin/sh
# 为 native_sim eth_native_tap 创建的 zeth 接口配置 IP.
#
# 注意: zeth 由 zephyr.exe 启动时经 /dev/net/tun 的 TUNSETIFF 自动创建,
# 故本脚本须在 zephyr.exe 启动之后运行 (见 README 运行顺序).
IFACE="${IFACE:-zeth}"
HOST_IP="${HOST_IP:-192.168.1.1}"

if ! ip link show "$IFACE" >/dev/null 2>&1; then
	echo "error: $IFACE not found. Start zephyr.exe first (in another terminal)." >&2
	exit 1
fi

# 接口已由驱动创建, 此处只分配 IP 并 up (carrier up 后设备侧触发 NET_EVENT_IF_UP)
sudo ip addr add "$HOST_IP/24" dev "$IFACE" 2>/dev/null || true
sudo ip link set "$IFACE" up
echo "$IFACE configured: $HOST_IP/24 (device IP 192.168.1.100)"
