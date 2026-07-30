#!/bin/bash
# gw_sim nsos 模式运行验证 (socket 走主机网络, 不需 sudo/zeth).
# 用法: 在 gw_sim 目录执行  ./scripts/run_test.sh
set -e
cd "$(dirname "$0")/.."

EXE=./build/zephyr/zephyr.exe
LOG=/tmp/gw_sim.log
DEV=${DEV:-127.0.0.1}   # nsos: 设备 bind 0.0.0.0, 用 localhost 访问

[ -x "$EXE" ] || { echo "error: 先 'west build -b native_sim'"; exit 1; }
command -v socat >/dev/null || { echo "error: apt install socat"; exit 1; }

echo "==> 清理可能残留的 zephyr.exe..."
pkill -f "zephyr.exe" 2>/dev/null || true
sleep 1

echo "==> 启动 zephyr.exe (nsos, 主机网络)..."
"$EXE" > "$LOG" 2>&1 &
EXE_PID=$!
# SIGINT 让 native_sim 优雅退出 (flush 日志), 再 SIGKILL 兜底
cleanup() {
	kill -INT "$EXE_PID" 2>/dev/null || true
	sleep 1
	kill "$EXE_PID" 2>/dev/null || true
}
trap cleanup EXIT
sleep 3

echo "==> 设备就绪日志:"
grep -E "listening|gw_sim ready|socket.*=> fd|error" "$LOG" || true
echo

# GET_VERSION / GET_CONFIG: 需读响应, 用 socat (-T 1 无响应 1s 退)
echo "==> [GET_VERSION] 期望 06 30 2e 31 2e 30 2d 64 65 76 :"
timeout 2 bash -c "printf '\006' | socat -T 1 - UDP-DATAGRAM:${DEV}:9200" 2>/dev/null | xxd

echo "==> [GET_CONFIG] 期望 11 字节 05 4c 00*5 23 82 23 f0 :"
timeout 2 bash -c "printf '\005' | socat -T 1 - UDP-DATAGRAM:${DEV}:9200" 2>/dev/null | xxd

# 数据帧 / 固件升级: 只发不读, 用 bash /dev/udp (避免 socat 等响应卡住)

# 先等 data 端口就绪 (data 线程 socket/bind 可能比 config 晚)
for i in $(seq 1 20); do
	grep -q "data port 9090 listening" "$LOG" && break
	sleep 0.3
done

echo "==> [数据帧 0x263] 触发 rf24 stub send:"
printf '\002\143ABCDE' > /dev/udp/${DEV}/9090
sleep 1

echo "==> [固件升级 0x10/0x11/0x12]:"
printf '\020'           > /dev/udp/${DEV}/9200
printf '\0211234567890' > /dev/udp/${DEV}/9200
printf '\021abcdefgh'   > /dev/udp/${DEV}/9200
printf '\022'           > /dev/udp/${DEV}/9200
sleep 2

echo
echo "==> 完整设备日志核对 (期望 6 项):"
grep -E "listening|socket.*=> fd|rf24 stub send|FW upgrade|FW chunk|FW end|total" "$LOG" || true

echo
echo "==> 验证完成. 完整日志: $LOG"
