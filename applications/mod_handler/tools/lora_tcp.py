#!/usr/bin/env python3
"""
LoRa Gateway TCP Tool — 命令行版

连接 USR-LG210-L 网关 TCP 端口，收发/解析 LoRa 数据帧。
支持遥测数据显示、ACK、手动发送原始帧、RSSI 查询。

协议: [NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE]
发送时在帧头额外添加 4B 定向 NID (Gateway Prefix)。

用法:
    python lora_tcp.py <ip> <port> [--nid HEX_NID] [--no-auto-ack]
"""

import socket
import struct
import sys
import threading
import time
import select

# ── 帧协议常量 ──
FRAME_NID_SIZE = 4
FRAME_LEN_SIZE = 2
FRAME_CRC_SIZE = 2
FRAME_HEADER_SIZE = FRAME_NID_SIZE + FRAME_LEN_SIZE
FRAME_OVERHEAD = FRAME_HEADER_SIZE + FRAME_CRC_SIZE
GATEWAY_PREFIX = 4

# 数据类型
DATA_HANDLER = 0x01
DATA_TEST = 0x02
DATA_RSSI = 0x03
DATA_ACK = 0x04

TYPE_NAMES = {
    DATA_HANDLER: "HANDLER",
    DATA_TEST: "TEST",
    DATA_RSSI: "RSSI",
    DATA_ACK: "ACK",
}

# ── CRC16-CCITT (与 Zephyr crc16_ccitt 一致) ──


def crc16_ccitt(seed: int, data: bytes) -> int:
    crc = seed
    for b in data:
        e = (crc ^ b) & 0xFF
        f = (e ^ (e << 4)) & 0xFF
        crc = ((crc >> 8) ^ (f << 8) ^ (f << 3) ^ (f >> 4)) & 0xFFFF
    return crc


# ── 帧组/解 ──


def build_frame(nid: int, payload: bytes = b"") -> bytes:
    nid_b = struct.pack(">I", nid)
    len_b = struct.pack(">H", len(payload))
    header = nid_b + len_b + payload
    crc = struct.pack(">H", crc16_ccitt(0, header))
    return header + crc


def wrap_for_gateway(gateway_nid: int, frame: bytes) -> bytes:
    return struct.pack(">I", gateway_nid) + frame


def parse_frames(buf: bytes):
    """从 buf 中提取所有完整帧，返回 (frames_list, remaining_bytes)"""
    frames = []
    pos = 0
    while len(buf) - pos >= FRAME_OVERHEAD:
        nid = struct.unpack(">I", buf[pos : pos + 4])[0]
        data_len = struct.unpack(">H", buf[pos + 4 : pos + 6])[0]
        total = FRAME_HEADER_SIZE + data_len + FRAME_CRC_SIZE

        if total > 2048:
            pos += 1
            print(f"  [WARN] Invalid frame header at offset {pos - 1}, re-syncing")
            continue

        if len(buf) - pos < total:
            break

        frame_data = buf[pos : pos + total]
        calc_crc = crc16_ccitt(0, buf[pos : pos + FRAME_HEADER_SIZE + data_len])
        rx_crc = struct.unpack(
            ">H", buf[pos + FRAME_HEADER_SIZE + data_len : pos + total]
        )[0]

        if calc_crc != rx_crc:
            print(
                f"  [WARN] CRC error at offset {pos}: calc={calc_crc:04X} rx={rx_crc:04X}"
            )
            pos += total
            continue

        frames.append(
            {
                "nid": nid,
                "data_len": data_len,
                "payload": buf[
                    pos + FRAME_HEADER_SIZE : pos + FRAME_HEADER_SIZE + data_len
                ],
                "raw": frame_data,
            }
        )
        pos += total

    return frames, buf[pos:]


# ── 帧内容解析 ──


def describe_frame(frame: dict) -> str:
    nid = frame["nid"]
    payload = frame["payload"]
    plen = frame["data_len"]
    hex_raw = frame["raw"].hex(" ").upper()

    if plen == 0:
        return f"[{nid:08X}] ACK (empty payload)  RAW: {hex_raw}"

    dtype = payload[0]
    body = payload[1:]
    type_name = TYPE_NAMES.get(dtype, f"0x{dtype:02X}")

    if dtype == DATA_ACK:
        return f"[{nid:08X}] ACK  RAW: {hex_raw}"

    if (
        dtype == DATA_HANDLER
        and len(body) == 8
        and body[5] == 0xFF
        and body[6] == 0xFF
        and body[7] == 0xFF
    ):
        x = struct.unpack(">h", body[0:2])[0] / 10.0
        y = struct.unpack(">h", body[2:4])[0] / 10.0
        btn = "Pressed" if (body[4] & 0x01) == 0 else "Released"
        return f"[{nid:08X}] Telemetry: X={x:.1f} Y={y:.1f} Btn={btn}  RAW: {hex_raw}"

    if dtype == DATA_RSSI:
        if len(body) >= 1:
            rssi_level = body[0]
            level_desc = {4: "Excellent", 3: "Good", 2: "Fair", 1: "Poor"}.get(
                rssi_level, "Unknown"
            )
            return f"[{nid:08X}] RSSI Response: level={rssi_level} ({level_desc})  RAW: {hex_raw}"
        else:
            return f"[{nid:08X}] RSSI Request  RAW: {hex_raw}"

    body_hex = body.hex(" ").upper() if body else ""
    return f"[{nid:08X}] {type_name} ({plen - 1}B): {body_hex}  RAW: {hex_raw}"


# ── 发送辅助 ──


def send_ack(sock: socket.socket, gateway_nid: int, nid: int, auto_ack_event=None):
    ack_payload = bytes([DATA_ACK])
    frame = build_frame(nid, ack_payload)
    pkt = wrap_for_gateway(gateway_nid, frame)
    sock.sendall(pkt)
    print(f"  >> TX ACK [{nid:08X}]")


def send_data(sock: socket.socket, gateway_nid: int, nid: int, data: bytes):
    frame = build_frame(nid, data)
    pkt = wrap_for_gateway(gateway_nid, frame)
    sock.sendall(pkt)
    print(f"  >> TX Data [{nid:08X}] {len(data)}B: {data.hex(' ').upper()}")


def send_rssi_request(sock: socket.socket, gateway_nid: int, nid: int):
    rssi_payload = bytes([DATA_RSSI])
    frame = build_frame(nid, rssi_payload)
    pkt = wrap_for_gateway(gateway_nid, frame)
    sock.sendall(pkt)
    print(f"  >> TX RSSI Request [{nid:08X}]")


# ── 接收线程 ──


def recv_thread(
    sock: socket.socket,
    auto_ack: bool,
    gateway_nid: int,
    stop_event: threading.Event,
    rssi_level: list,
):
    rx_buf = bytearray()
    stats = {"rx": 0, "tx_ack": 0, "err": 0}
    last_nid = 0

    while not stop_event.is_set():
        try:
            ready, _, _ = select.select([sock], [], [], 0.5)
        except Exception:
            break
        if not ready:
            continue

        try:
            data = sock.recv(4096)
        except Exception as e:
            print(f"\n  [ERROR] recv: {e}")
            break

        if not data:
            print("\n  Connection closed by remote")
            break

        rx_buf.extend(data)

        frames, remaining = parse_frames(bytes(rx_buf))
        rx_buf = bytearray(remaining)

        for frame in frames:
            nid = frame["nid"]
            payload = frame["payload"]
            plen = frame["data_len"]

            last_nid = nid
            stats["rx"] += 1

            ts = time.strftime("%H:%M:%S")
            desc = describe_frame(frame)
            print(f"  [{ts}] RX #{stats['rx']} {desc}")

            if auto_ack and plen > 0 and payload[0] in (DATA_HANDLER, DATA_TEST):
                send_ack(sock, gateway_nid, nid)
                stats["tx_ack"] += 1

            if plen > 0 and payload[0] == DATA_RSSI and len(payload) == 1:
                rssi_resp = bytes([DATA_RSSI, stats.get("rssi_level", 4)])
                frame = build_frame(nid, rssi_resp)
                pkt = wrap_for_gateway(gateway_nid, frame)
                sock.sendall(pkt)
                print(
                    f"  >> TX RSSI Response [{nid:08X}] level={stats.get('rssi_level', 4)}"
                )

    print(
        f"\n  --- Stats: RX={stats['rx']} TX_ACK={stats['tx_ack']} ERR={stats['err']} ---"
    )
    stop_event.set()


# ── 交互命令 ──


def print_help():
    print("""
Commands:
  h, help              Show this help
  s, send HEX...       Send raw data frame (hex bytes, e.g. "s 01 02 FF")
  a, ack               Send ACK to last NID
  r, rssi              Send RSSI query to last NID
  rl [1-4]             Set RSSI level for auto-response (1=Poor 2=Fair 3=Good 4=Excellent)
  t, telemetry         Send telemetry test frame (X=0 Y=0 Btn=0)
  n, nid [HEX]         Set/get NID filter (0 = accept all)
  p, prefix [HEX]      Set/get gateway prefix NID
  q, quit              Disconnect and exit
""")


def parse_hex(s: str) -> bytes:
    parts = s.strip().split()
    return bytes(int(x, 16) for x in parts)


# ── 主程序 ──


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <ip> <port> [--nid HEX] [--no-auto-ack]")
        print(f"Example: {sys.argv[0]} 192.168.2.100 1234 --nid 00000001")
        sys.exit(1)

    ip = sys.argv[1]
    port = int(sys.argv[2])
    nid_filter = 0
    auto_ack = True

    i = 3
    while i < len(sys.argv):
        if sys.argv[i] == "--nid" and i + 1 < len(sys.argv):
            nid_filter = int(sys.argv[i + 1], 16)
            i += 2
        elif sys.argv[i] == "--no-auto-ack":
            auto_ack = False
            i += 1
        else:
            i += 1

    gateway_nid = nid_filter if nid_filter else 0x00000001
    last_nid = nid_filter
    rssi_level = [4]  # mutable list so thread can read; default Excellent

    print(f"Connecting to {ip}:{port} ...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    try:
        sock.connect((ip, port))
    except Exception as e:
        print(f"Connect failed: {e}")
        sys.exit(1)

    sock.settimeout(None)
    print(
        f"Connected! NID filter={nid_filter:08X} Gateway={gateway_nid:08X} AutoACK={auto_ack}"
    )
    print("Type 'h' for help.\n")

    stop_event = threading.Event()
    t = threading.Thread(
        target=recv_thread, args=(sock, auto_ack, gateway_nid, stop_event), daemon=True
    )
    t.start()

    try:
        while not stop_event.is_set():
            try:
                line = input("").strip()
            except (EOFError, KeyboardInterrupt):
                break

            if not line:
                continue

            parts = line.split(None, 1)
            cmd = parts[0].lower()
            arg = parts[1] if len(parts) > 1 else ""

            if cmd in ("q", "quit", "exit"):
                break
            elif cmd in ("h", "help"):
                print_help()
            elif cmd in ("s", "send"):
                try:
                    data = parse_hex(arg)
                    target = last_nid or gateway_nid
                    send_data(sock, gateway_nid, target, data)
                except Exception as e:
                    print(f"  Error: {e}")
            elif cmd in ("a", "ack"):
                target = last_nid or gateway_nid
                ack_payload = bytes([DATA_ACK])
                frame = build_frame(target, ack_payload)
                pkt = wrap_for_gateway(gateway_nid, frame)
                sock.sendall(pkt)
                print(f"  >> TX ACK [{target:08X}]")
            elif cmd in ("r", "rssi"):
                target = last_nid or gateway_nid
                send_rssi_request(sock, gateway_nid, target)
            elif cmd in ("t", "telemetry"):
                target = last_nid or gateway_nid
                x = int(0).to_bytes(2, "big", signed=True)
                y = int(0).to_bytes(2, "big", signed=True)
                telemetry = (
                    bytes([DATA_HANDLER]) + x + y + bytes([0x00, 0xFF, 0xFF, 0xFF])
                )
                frame = build_frame(target, telemetry)
                pkt = wrap_for_gateway(gateway_nid, frame)
                sock.sendall(pkt)
                print(f"  >> TX Telemetry [{target:08X}]")
            elif cmd in ("n", "nid"):
                if arg:
                    try:
                        nid_filter = int(arg, 16)
                        last_nid = nid_filter
                        print(f"  NID filter set to {nid_filter:08X}")
                    except ValueError:
                        print("  Invalid hex")
                else:
                    print(f"  NID filter: {nid_filter:08X}")
            elif cmd in ("p", "prefix"):
                if arg:
                    try:
                        gateway_nid = int(arg, 16)
                        print(f"  Gateway prefix set to {gateway_nid:08X}")
                    except ValueError:
                        print("  Invalid hex")
                else:
                    print(f"  Gateway prefix: {gateway_nid:08X}")
            else:
                print(f"  Unknown command: {cmd}. Type 'h' for help.")
    except Exception:
        pass

    stop_event.set()
    sock.close()
    t.join(timeout=2)
    print("Disconnected.")


if __name__ == "__main__":
    main()
