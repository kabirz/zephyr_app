#!/usr/bin/env python3
"""
gw_sim 测试脚本 (nsos 模式) — 只测试, 不启动设备.
设备需先在另一终端启动:
    cd applications/gw_sim && ./build/zephyr/zephyr.exe

用法:
    python3 scripts/test_gw_sim.py                      # 不验证设备日志
    python3 scripts/test_gw_sim.py --log /tmp/gw_sim.log # 同时校验设备日志(数据帧/FW)
"""
import argparse
import socket
import struct
import sys
import time

HOST = "127.0.0.1"          # nsos: 设备 bind 0.0.0.0, 用 localhost 访问
BCAST = "255.255.255.255"
CONFIG_PORT = 9200
DATA_PORT = 9090
TIMEOUT = 2.0

# 命令字节 (udp.c::enum udp_cmd)
GET_VERSION, GET_CONFIG = 0x06, 0x05
SET_IP, SET_MASK, SET_GW, SET_PORT = 0x01, 0x02, 0x03, 0x04
SET_RF24_CH, SET_RF24_ADDR = 0x07, 0x08
FW_START, FW_DATA, FW_END = 0x10, 0x11, 0x12

passes = 0
fails = 0


def check(name, cond, detail=""):
    global passes, fails
    if cond:
        passes += 1
        print(f"  [PASS] {name}")
    else:
        fails += 1
        print(f"  [FAIL] {name}  {detail}")


def udp(port, payload, host=HOST, expect=True):
    """发 payload 到 host:port, 返回响应 (或 None)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(TIMEOUT)
    try:
        s.sendto(payload, (host, port))
        if expect:
            data, _ = s.recvfrom(1024)
            return data
    except socket.timeout:
        return None
    finally:
        s.close()
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=None,
                    help="设备日志文件路径 (用于验证数据帧/FW 日志, 可选)")
    args = ap.parse_args()

    def log_contains(pattern, t=2.0):
        """返回 True/False (找到/没找到), None (无日志文件, 跳过)."""
        if not args.log:
            return None
        end = time.time() + t
        while time.time() < end:
            try:
                with open(args.log) as f:
                    if pattern in f.read():
                        return True
            except OSError:
                return None
            time.sleep(0.2)
        return False

    # 探测设备是否在运行
    r = udp(CONFIG_PORT, bytes([GET_VERSION]))
    if not r:
        print("error: 设备未运行或 config(9200) 无响应.")
        print("       先在另一终端启动: ./build/zephyr/zephyr.exe")
        sys.exit(1)
    print("==> 设备就绪, 开始测试\n")

    # ---------- 配置命令 (9200) ----------
    print("[配置命令]")

    r = udp(CONFIG_PORT, bytes([GET_VERSION]))
    check("GET_VERSION", r and r[0] == GET_VERSION and r[1:] == b"0.1.0-dev",
          f"got {r}")

    # GET_CONFIG: [0x05][ch 1][addr 5][data_port 2 BE][config_port 2 BE] = 11B
    r = udp(CONFIG_PORT, bytes([GET_CONFIG]))
    ok = r and len(r) == 11 and r[0] == GET_CONFIG
    if ok:
        ch = r[1]
        dport = struct.unpack(">H", r[7:9])[0]
        cport = struct.unpack(">H", r[9:11])[0]
        ok = ch == 76 and dport == 9090 and cport == 9200
    check("GET_CONFIG", ok, f"got {r.hex() if r else None}")

    r = udp(CONFIG_PORT, bytes([SET_IP]) + bytes([192, 168, 1, 64]))
    check("SET_IP", r and r[0] == SET_IP and r[1:] == b"192.168.1.64", f"got {r}")

    r = udp(CONFIG_PORT, bytes([SET_MASK]) + bytes([255, 255, 255, 0]))
    check("SET_MASK", r and r[0] == SET_MASK and r[1:] == b"255.255.255.0", f"got {r}")

    r = udp(CONFIG_PORT, bytes([SET_GW]) + bytes([192, 168, 1, 1]))
    check("SET_GW", r and r[0] == SET_GW and r[1:] == b"192.168.1.1", f"got {r}")

    r = udp(CONFIG_PORT, bytes([SET_PORT]) + struct.pack(">H", 9091))
    check("SET_PORT", r and r[0] == SET_PORT and r[1:] == struct.pack(">H", 9091),
          f"got {r}")

    r = udp(CONFIG_PORT, bytes([SET_RF24_CH, 100]))
    check("SET_RF24_CH", r and r[0] == SET_RF24_CH and r[1] == 100, f"got {r}")

    addr = bytes([1, 2, 3, 4, 5])
    r = udp(CONFIG_PORT, bytes([SET_RF24_ADDR]) + addr)
    check("SET_RF24_ADDR", r and r[0] == SET_RF24_ADDR and r[1:] == addr, f"got {r}")

    r = udp(CONFIG_PORT, bytes([GET_VERSION]), host=BCAST)
    check("广播 GET_VERSION", r and r[1:] == b"0.1.0-dev", f"got {r}")

    # ---------- 数据帧 (9090, 无响应) ----------
    print("\n[数据帧转发]")
    udp(DATA_PORT, struct.pack(">H", 0x263) + b"ABCDE", expect=False)
    time.sleep(0.5)
    lc = log_contains("rf24 stub send: id=0x263")
    if lc is None:
        print("  [SKIP] 数据帧日志验证 (未指定 --log; 请在设备终端确认 'rf24 stub send: id=0x263')")
    else:
        check("数据帧 0x263 → rf24 stub", lc, "日志无 rf24 stub send")

    # ---------- 固件升级 ----------
    print("\n[固件升级]")
    r = udp(CONFIG_PORT, bytes([FW_START]))
    check("FW_START", r == bytes([FW_START]) + b"ok", f"got {r}")

    total = 0
    for chunk in [b"1234567890", b"abcdefgh"]:
        r = udp(CONFIG_PORT, bytes([FW_DATA]) + chunk)
        total += len(chunk)
        check(f"FW_DATA +{len(chunk)}", r == bytes([FW_DATA]) + b"ok", f"got {r}")

    r = udp(CONFIG_PORT, bytes([FW_END]))
    check(f"FW_END (total {total})", r == bytes([FW_END]) + b"ok", f"got {r}")
    lc = log_contains(f"total {total} bytes")
    if lc is not None:
        check(f"FW 日志 total {total} bytes", lc)

    print(f"\n==> 结果: {passes} passed, {fails} failed")
    sys.exit(0 if fails == 0 else 1)


if __name__ == "__main__":
    main()
