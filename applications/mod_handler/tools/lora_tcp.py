#!/usr/bin/env python3
"""
LoRa Gateway Tool — 命令行版 (TCP + UDP)

连接 USR-LG210-L 网关，TCP 收发/解析 LoRa 数据帧，UDP 设备发现与配置。

数据帧格式 (TX): [Gateway Prefix 4B][0xAA][0x55][NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE][\\r\\n]
接收格式 (RX):   [0xAA][0x55][NID 4B BE][Length 2B BE][Data NB][CRC16-CCITT 2B BE][\\r\\n]
UDP 配置协议:    USR1566{JSON}USR1566 (port 1566)

用法:
    python lora_tcp.py <ip> <port> [--nid HEX_NID] [--no-auto-ack]
    python lora_tcp.py --no-connect [--nid HEX_NID]  (启动不自动连接)
"""

import json
import socket
import struct
import sys
import threading
import time
import select
try:
    import readline  # noqa: F401 — 启用上下键历史 + 行编辑
except ImportError:
    pass

# ── 帧协议常量 ──
FRAME_NID_SIZE = 4
FRAME_LEN_SIZE = 2
FRAME_CRC_SIZE = 2
FRAME_HEADER_SIZE = FRAME_NID_SIZE + FRAME_LEN_SIZE
FRAME_OVERHEAD = FRAME_HEADER_SIZE + FRAME_CRC_SIZE
GATEWAY_PREFIX = 4

# 数据帧封装: [0xAA][0x55][content][\r\n]
FRAME_HDR = b'\xAA\x55'
FRAME_FTR = b'\r\n'

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

# UDP 常量
UDP_PORT = 1566
UDP_TIMEOUT = 5


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
    """构建统一帧: [NID 4B][Length 2B][Data][CRC 2B]"""
    nid_b = struct.pack(">I", nid)
    len_b = struct.pack(">H", len(payload))
    header = nid_b + len_b + payload
    crc = struct.pack(">H", crc16_ccitt(0, header))
    return header + crc


def wrap_for_gateway(gateway_nid: int, frame: bytes) -> bytes:
    """TX 封装: [GW Prefix 4B][0xAA][0x55][统一帧][\r\n]"""
    return struct.pack(">I", gateway_nid) + FRAME_HDR + frame + FRAME_FTR


def parse_frames(buf: bytes):
    """从 buf 中通过 0xAA 0x55 + \\r\\n 边界检测提取完整帧

    帧格式: [0xAA][0x55][content][\\r\\n]
    content 为统一帧: [NID 4B BE][Length 2B BE][Data NB][CRC16 2B BE]
    """
    frames = []
    pos = 0
    while pos + 4 <= len(buf):
        if buf[pos] != 0xAA or buf[pos + 1] != 0x55:
            pos += 1
            continue
        tail = buf.find(b"\r\n", pos + 2)
        if tail < 0:
            break

        content = buf[pos + 2 : tail]
        frame_end = tail + 2

        if len(content) < FRAME_OVERHEAD:
            pos += 1
            continue

        nid = struct.unpack(">I", content[0:4])[0]
        data_len = struct.unpack(">H", content[4:6])[0]
        total = FRAME_HEADER_SIZE + data_len + FRAME_CRC_SIZE

        if total > 2048:
            pos += 1
            print(f"  [WARN] Invalid frame header at offset {pos - 1}, re-syncing")
            continue

        if len(content) < total:
            pos += 1
            continue

        frame_data = content[:total]
        calc_crc = crc16_ccitt(0, content[: FRAME_HEADER_SIZE + data_len])
        rx_crc = struct.unpack(
            ">H", content[FRAME_HEADER_SIZE + data_len : total]
        )[0]

        if calc_crc != rx_crc:
            print(
                f"  [WARN] CRC error at offset {pos}: calc={calc_crc:04X} rx={rx_crc:04X}"
            )
            pos = frame_end
            continue

        frames.append(
            {
                "nid": nid,
                "data_len": data_len,
                "payload": content[
                    FRAME_HEADER_SIZE : FRAME_HEADER_SIZE + data_len
                ],
                "raw": frame_data,
            }
        )
        pos = frame_end

    return frames, buf[pos:]


def describe_frame(frame: dict) -> str:
    """格式化帧描述"""
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


# ── UDP 辅助 ──

def udp_wrap_json(data: dict) -> bytes:
    """构建 USR1566{json}USR1566 格式 payload"""
    json_str = json.dumps(data, separators=(",", ":"))
    return f"USR1566{json_str}USR1566".encode()


def get_local_ips() -> list:
    """获取本机所有活跃 IPv4 地址"""
    ips = []
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips and not ip.startswith("127."):
                ips.append(ip)
    except Exception:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        if ip not in ips:
            ips.append(ip)
        s.close()
    except Exception:
        pass
    return ips


def rssi_to_level(rssi: int) -> int:
    """RSSI 值转信号等级 (1-4)"""
    if rssi >= -80:
        return 4
    if rssi >= -90:
        return 3
    if rssi >= -100:
        return 2
    return 1


def udp_send_broadcast(payload: bytes, timeout: float = UDP_TIMEOUT) -> list:
    """在所有网卡广播 payload，等待响应，返回 [(from_ip, data), ...]"""
    results = []
    local_ips = get_local_ips()
    if not local_ips:
        print("  [WARN] No active IPv4 interfaces found")
        return results

    dest = ("255.255.255.255", UDP_PORT)
    socks = []

    for ip in local_ips:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            sock.bind((ip, 0))
            sock.sendto(payload, dest)
            print(f"  TX ({ip}) -> broadcast")
        except Exception as e:
            print(f"  [WARN] sendto failed on {ip}: {e}")
            sock.close()
            continue
        sock.setblocking(False)
        socks.append(sock)

    if not socks:
        print("  [WARN] Failed to send on any interface")
        return results

    start = time.monotonic()
    buf = bytearray(4096)

    while time.monotonic() - start < timeout:
        remaining = timeout - (time.monotonic() - start)
        if remaining <= 0:
            break
        read_fds, _, _ = select.select(socks, [], [], min(remaining, 0.2))
        for sock in read_fds:
            try:
                nbytes, addr = sock.recvfrom_into(buf, len(buf))
                if nbytes > 0:
                    results.append((addr[0], bytes(buf[:nbytes])))
            except Exception:
                pass
        if results:
            break

    for sock in socks:
        sock.close()
    return results


def udp_send_unicast(ip: str, payload: bytes, timeout: float = UDP_TIMEOUT) -> list:
    """单播到指定 IP，等待响应"""
    results = []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(payload, (ip, UDP_PORT))
        print(f"  TX -> {ip}")
    except Exception as e:
        print(f"  [WARN] sendto {ip} failed: {e}")
        sock.close()
        return results

    sock.setblocking(False)
    start = time.monotonic()
    buf = bytearray(4096)

    while time.monotonic() - start < timeout:
        remaining = timeout - (time.monotonic() - start)
        if remaining <= 0:
            break
        read_fds, _, _ = select.select([sock], [], [], min(remaining, 0.2))
        for _ in read_fds:
            try:
                nbytes, addr = sock.recvfrom_into(buf, len(buf))
                if nbytes > 0:
                    results.append((addr[0], bytes(buf[:nbytes])))
            except Exception:
                pass
        if results:
            break

    sock.close()
    return results


def udp_send(st, payload: bytes) -> list:
    """根据设备状态选择单播或广播"""
    if st.dev_addr:
        return udp_send_unicast(st.dev_addr, payload)
    return udp_send_broadcast(payload)


def extract_at_value(text: str, prefix: str) -> str:
    """从 AT 响应中提取 prefix 之后的值"""
    p = text.find(prefix)
    if p < 0:
        return ""
    p += len(prefix)
    end = p
    while end < len(text) and text[end] not in ("\r", "\n", " "):
        end += 1
    return text[p:end]


# ── 全局状态 ──

class LoRaState:
    def __init__(self):
        self.tcp_sock = None
        self.gateway_nid = 0x00000001
        self.nid_filter = 0
        self.auto_ack = True
        self.connected = False
        self.quit_event = threading.Event()       # 整个程序退出
        self.disconnected = threading.Event()      # TCP 断连信号 (recv_thread 设置)
        self.disconnected.set()                    # 初始为"已断开"
        self.recv_thread = None
        self.rx_buf = bytearray()
        self.stats = {"rx": 0, "tx": 0, "tx_ack": 0, "err": 0}
        self.last_nid = 0
        self.rssi_level = 4
        self.pending_rssi_nid = 0
        self.rssi_lock = threading.Lock()
        # 自动重连
        self.auto_reconnect = True
        self.reconnect_interval = 3               # 秒
        self.reconnect_max = 0                    # 0 = 无限重试
        self.last_ip = ""
        self.last_port = 0
        self.manual_disconnect = False            # 用户主动断开时阻止自动重连
        self.reconnect_thread = None
        # UDP 设备信息
        self.dev_mac = ""
        self.dev_addr = ""  # UDP 源 IP
        self.dev_ip = ""
        self.dev_sm = ""
        self.dev_gw = ""
        self.dev_gwid = ""
        self.dev_name = ""
        self.dev_sw = ""


# ── TCP 连接管理 ──

def tcp_connect(st: LoRaState, ip: str, port: int):
    """建立 TCP 连接，启动接收线程"""
    if st.connected:
        print("  Already connected. Use 'disconnect' first.")
        return False

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((ip, port))
        sock.settimeout(None)
    except Exception as e:
        print(f"  Connect failed: {e}")
        return False

    st.tcp_sock = sock
    st.connected = True
    st.rx_buf = bytearray()
    st.disconnected.clear()
    st.manual_disconnect = False
    st.last_ip = ip
    st.last_port = port

    st.recv_thread = threading.Thread(
        target=recv_thread, args=(st,), daemon=True
    )
    st.recv_thread.start()

    print(f"  Connected to {ip}:{port}")
    print(
        f"  NID filter={st.nid_filter:08X} Gateway={st.gateway_nid:08X} "
        f"AutoACK={st.auto_ack}"
    )
    return True


def tcp_disconnect(st: LoRaState):
    """用户主动断开 TCP 连接"""
    if not st.connected and st.tcp_sock is None:
        print("  Not connected")
        return

    st.manual_disconnect = True
    st.connected = False
    if st.tcp_sock:
        try:
            st.tcp_sock.close()
        except Exception:
            pass
        st.tcp_sock = None

    # 等待接收线程结束
    if st.recv_thread and st.recv_thread.is_alive():
        st.recv_thread.join(timeout=2)
    st.recv_thread = None
    st.rx_buf = bytearray()
    st.disconnected.set()
    print("  Disconnected")


# ── TCP 发送辅助 ──

def tcp_send_frame(st: LoRaState, nid: int, payload: bytes):
    """TCP 发送: [GW Prefix][0xAA][0x55][统一帧][\r\n]"""
    if not st.connected or not st.tcp_sock:
        print("  [ERROR] Not connected")
        return
    frame = build_frame(nid, payload)
    pkt = wrap_for_gateway(st.gateway_nid, frame)
    try:
        st.tcp_sock.sendall(pkt)
    except Exception as e:
        print(f"  [ERROR] send failed: {e}")
        st.connected = False
        return
    st.stats["tx"] += 1


def send_ack_tcp(st: LoRaState, nid: int):
    tcp_send_frame(st, nid, bytes([DATA_ACK]))
    print(f"  >> TX ACK [{nid:08X}]")


def send_rssi_response_tcp(st: LoRaState, nid: int, level: int):
    tcp_send_frame(st, nid, bytes([DATA_RSSI, level]))
    print(f"  >> TX RSSI Response [{nid:08X}] level={level}")


def query_ninfo_udp(st: LoRaState, nid: int):
    """UDP 查询 NINFO, 提取 RSSI, 通过 TCP 回复"""
    mac = st.dev_mac or "D4AD20ED63C4"
    payload_data = {
        "VER": "1.0", "MSG": "GETPARA", "TYPE": "AT",
        "CMD": "AT+NINFO?\r\n",
        "USER": "admin", "PSW": "admin", "MAC": mac,
    }
    payload = udp_wrap_json(payload_data)
    responses = udp_send(st, payload)

    for from_ip, raw in responses:
        raw_str = raw.decode("utf-8", errors="replace")
        start = raw_str.find("{")
        end = raw_str.rfind("}")
        if start < 0 or end <= start:
            continue
        try:
            root = json.loads(raw_str[start : end + 1])
        except json.JSONDecodeError:
            continue
        cmd_obj = root.get("CMD")
        if not isinstance(cmd_obj, str):
            continue
        # 解析 NINFO: 网络id,节点id,?,SNR,RSSI,...
        p = cmd_obj.find("+NINFO:")
        if p < 0:
            continue
        info = cmd_obj[p + 7 :]
        fields = info.split(",")
        rssi_val = -120
        if len(fields) >= 5:
            try:
                rssi_val = int(fields[4])
            except ValueError:
                pass
        level = rssi_to_level(rssi_val)
        print(f"  NINFO RSSI={rssi_val} -> level={level}")
        send_rssi_response_tcp(st, nid, level)
        return  # 只需一次成功


# ── TCP 接收线程 ──

def recv_thread(st: LoRaState):
    """TCP 接收线程 — 断连时仅设置状态，不退出主循环"""
    while not st.quit_event.is_set():
        try:
            ready, _, _ = select.select([st.tcp_sock], [], [], 0.5)
        except Exception:
            break
        if not ready:
            continue

        try:
            data = st.tcp_sock.recv(4096)
        except Exception as e:
            print(f"\n  [ERROR] recv: {e}")
            break

        if not data:
            print("\n  Connection closed by remote")
            break

        st.rx_buf.extend(data)
        frames, remaining = parse_frames(bytes(st.rx_buf))
        st.rx_buf = bytearray(remaining)

        for frame in frames:
            nid = frame["nid"]
            payload = frame["payload"]
            plen = frame["data_len"]

            st.last_nid = nid
            st.stats["rx"] += 1

            ts = time.strftime("%H:%M:%S")
            desc = describe_frame(frame)
            print(f"  [{ts}] RX #{st.stats['rx']} {desc}")

            # 空 payload
            if plen == 0:
                continue

            dtype = payload[0]

            # RSSI 请求: 查询 NINFO 后回复
            if dtype == DATA_RSSI and len(payload) == 1:
                query_ninfo_udp(st, nid)
                continue

            # 自动 ACK
            if st.auto_ack and dtype in (DATA_HANDLER, DATA_TEST):
                send_ack_tcp(st, nid)
                st.stats["tx_ack"] += 1

    # 断连清理
    st.connected = False
    if st.tcp_sock:
        try:
            st.tcp_sock.close()
        except Exception:
            pass
        st.tcp_sock = None
    st.disconnected.set()


def reconnect_watcher(st: LoRaState):
    """后台自动重连线程 — 检测断连后按间隔重试"""
    while not st.quit_event.is_set():
        # 等待断连信号
        st.disconnected.wait(timeout=1.0)
        if st.quit_event.is_set():
            return
        if not st.disconnected.is_set():
            continue

        # 条件检查
        if not st.auto_reconnect:
            continue
        if st.manual_disconnect:
            continue
        if not st.last_ip or not st.last_port:
            continue

        # 等待 recv_thread 完全退出
        if st.recv_thread and st.recv_thread.is_alive():
            st.recv_thread.join(timeout=2)
            st.recv_thread = None

        # 重连循环
        attempt = 0
        while not st.quit_event.is_set():
            attempt += 1
            max_info = f"/{st.reconnect_max}" if st.reconnect_max > 0 else ""
            print(f"  Auto-reconnect ({attempt}{max_info}) to {st.last_ip}:{st.last_port} ...")

            # 等待间隔 (可被 quit_event 中断)
            if st.quit_event.wait(timeout=st.reconnect_interval):
                return

            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(5)
                sock.connect((st.last_ip, st.last_port))
                sock.settimeout(None)
            except Exception as e:
                print(f"  Reconnect failed: {e}")
                sock = None

            if sock:
                st.tcp_sock = sock
                st.connected = True
                st.rx_buf = bytearray()
                st.disconnected.clear()
                st.manual_disconnect = False

                st.recv_thread = threading.Thread(
                    target=recv_thread, args=(st,), daemon=True
                )
                st.recv_thread.start()

                print(f"  Reconnected to {st.last_ip}:{st.last_port}")
                break

            # 检查重连次数限制
            if st.reconnect_max > 0 and attempt >= st.reconnect_max:
                print(f"  Auto-reconnect gave up after {attempt} attempts")
                break


# ── UDP 配置命令 ──

def cmd_search(st: LoRaState):
    """搜索设备"""
    payload = udp_wrap_json({"VER": "1.0", "MSG": "SEARCH", "TYPE": "LORA"})
    print("  Searching...")
    responses = udp_send_broadcast(payload)

    if not responses:
        print("  No response (timeout 5s)")
        return

    for from_ip, raw in responses:
        raw_str = raw.decode("utf-8", errors="replace")
        start = raw_str.find("{")
        end = raw_str.rfind("}")
        if start < 0 or end <= start:
            print(f"  RX <- {from_ip}: (non-JSON)")
            continue
        try:
            root = json.loads(raw_str[start : end + 1])
        except json.JSONDecodeError:
            print(f"  RX <- {from_ip}: (parse error)")
            continue

        msg = root.get("MSG", "")
        if msg == "ACK-SEARCH":
            st.dev_addr = from_ip
            mac = root.get("MAC", "")
            dev = root.get("DEV", "")
            sver = root.get("SVER", "")
            if mac:
                st.dev_mac = mac
            if dev:
                st.dev_name = dev
            if sver:
                st.dev_sw = sver
            print(f"  Device found! IP={from_ip} MAC={mac} DEV={dev} SW={sver}")
        else:
            print(f"  RX <- {from_ip}: {json.dumps(root, indent=2)}")


def cmd_getnet(st: LoRaState):
    """获取网络参数"""
    if not st.dev_mac:
        print("  [ERROR] Please search devices first")
        return
    payload = udp_wrap_json({
        "VER": "1.0", "MSG": "GETPARA", "TYPE": "JSON", "CMD": "NETDEV",
        "USER": "admin", "PSW": "admin", "MAC": st.dev_mac,
    })
    responses = udp_send(st, payload)

    for from_ip, raw in responses:
        raw_str = raw.decode("utf-8", errors="replace")
        start = raw_str.find("{")
        end = raw_str.rfind("}")
        if start < 0 or end <= start:
            continue
        try:
            root = json.loads(raw_str[start : end + 1])
        except json.JSONDecodeError:
            continue

        msg = root.get("MSG", "")
        if msg == "ACK-GETPARA":
            cmd_obj = root.get("CMD")
            if isinstance(cmd_obj, dict):
                ip = cmd_obj.get("IP", "")
                sm = cmd_obj.get("SM", "")
                gw = cmd_obj.get("GW", "")
                if ip:
                    st.dev_ip = ip
                if sm:
                    st.dev_sm = sm
                if gw:
                    st.dev_gw = gw
                print(f"  Network: IP={ip} Mask={sm} GW={gw}")
            else:
                print(f"  RX <- {json.dumps(root, indent=2)}")


def cmd_send_at(st: LoRaState, at_cmd: str):
    """通过 UDP 发送 AT 指令 (查询用 GETPARA, 设置用 SETPARA)"""
    if not at_cmd:
        print("  [ERROR] Please enter AT command")
        return
    mac = st.dev_mac or "D4AD20ED63C4"
    # 确保 \r\n 结尾
    if not at_cmd.endswith("\r\n"):
        at_cmd = at_cmd.rstrip() + "\r\n"

    # 查询 (?) 用 GETPARA, 设置 (=) 或其他用 SETPARA
    stripped = at_cmd.strip()
    is_query = stripped.endswith("?")
    msg_type = "GETPARA" if is_query else "SETPARA"

    payload = udp_wrap_json({
        "VER": "1.0", "MSG": msg_type, "TYPE": "AT", "CMD": at_cmd,
        "USER": "admin", "PSW": "admin", "MAC": mac,
    })
    print(f"  TX AT ({msg_type}): {stripped}")
    responses = udp_send(st, payload)

    ack_msgs = ("ACK-GETPARA", "ACK-SETPARA")
    for from_ip, raw in responses:
        raw_str = raw.decode("utf-8", errors="replace")
        start = raw_str.find("{")
        end = raw_str.rfind("}")
        if start < 0 or end <= start:
            print(f"  RX <- {from_ip}: (non-JSON)")
            continue
        try:
            root = json.loads(raw_str[start : end + 1])
        except json.JSONDecodeError:
            continue

        msg = root.get("MSG", "")
        if msg in ack_msgs:
            cmd_obj = root.get("CMD")
            if isinstance(cmd_obj, str):
                print(f"  RX <- {cmd_obj.strip()}")
                _parse_at_response(st, cmd_obj)
            elif isinstance(cmd_obj, dict):
                print(f"  RX <- {json.dumps(cmd_obj, indent=2)}")
        else:
            print(f"  RX <- {json.dumps(root, indent=2)}")

    if not responses:
        print("  No response (timeout 5s)")


def _parse_at_response(st: LoRaState, val: str):
    """解析 AT 响应中的各字段"""
    # GWID
    p = val.find("GWID:")
    if p >= 0:
        v = val[p + 5 :].split("\r")[0].split("\n")[0].split(" ")[0]
        st.dev_gwid = v
        print(f"  GWID: {v}")

    # CSQ: +CSQ:<net>,<signal>
    p = val.find("+CSQ:")
    if p >= 0:
        try:
            parts = val[p + 5 :].split(",")
            net_val, sig_val = int(parts[0]), int(parts[1])
            net_str = {2: "2G", 3: "3G", 4: "4G"}.get(net_val, "?")
            print(f"  Signal: {net_str} ({sig_val})")
        except (ValueError, IndexError):
            pass

    # DHCP
    v = extract_at_value(val, "+DHCP:")
    if v:
        print(f"  DHCP: {v}")

    # IP
    v = extract_at_value(val, "+GWIP:")
    if v and v != "OK":
        st.dev_ip = v
        print(f"  IP: {v}")

    # Mask
    v = extract_at_value(val, "+MASK:")
    if v and v != "OK":
        st.dev_sm = v
        print(f"  Mask: {v}")

    # GW
    v = extract_at_value(val, "+GW:")
    if v and v != "OK":
        st.dev_gw = v
        print(f"  GW: {v}")

    # Option
    v = extract_at_value(val, "+OPTION:")
    if v and v != "OK":
        names = ["socket", "serial", "mqtt", "ali_cloud", "usr_cloud"]
        mode = int(v) if v.isdigit() else -1
        name = names[mode] if 0 <= mode <= 4 else "?"
        print(f"  Option: {name} ({v})")

    # NWMODE
    v = extract_at_value(val, "+NWMODE:")
    if v and v != "OK":
        print(f"  NWMODE: {v}")

    # TTMODE
    v = extract_at_value(val, "+TTMODE:")
    if v and v != "OK":
        print(f"  TTMODE: {v}")

    # WMODE
    v = extract_at_value(val, "+WMODE:")
    if v and v != "OK":
        print(f"  WMODE: {v}")

    # UPWID
    v = extract_at_value(val, "+UPWID:")
    if v and v != "OK":
        print(f"  UPWID: {v}")

    # CH<n>
    p = val.find("+CH")
    if p >= 0 and p + 3 < len(val) and val[p + 3].isdigit():
        colon = val.find(":", p + 3)
        if colon >= 0:
            v = val[colon + 1 :].split("\r")[0].split("\n")[0].split(" ")[0]
            if v != "OK":
                print(f"  CH{val[p + 3]}: {v}")

    # SPD<n>
    p = val.find("+SPD")
    if p >= 0 and p + 4 < len(val) and val[p + 4].isdigit():
        colon = val.find(":", p + 4)
        if colon >= 0:
            v = val[colon + 1 :].split("\r")[0].split("\n")[0].split(" ")[0]
            if v != "OK":
                print(f"  SPD{val[p + 4]}: {v}")

    # PWR<n>
    p = val.find("+PWR")
    if p >= 0 and p + 4 < len(val) and val[p + 4].isdigit():
        colon = val.find(":", p + 4)
        if colon >= 0:
            v = val[colon + 1 :].split("\r")[0].split("\n")[0].split(" ")[0]
            if v != "OK":
                print(f"  PWR{val[p + 4]}: {v}")


# ── 帮助 ──

HELP_TEXT = """
TCP Data Commands:
  connect <ip> <port>       Connect to gateway
  disconnect                Disconnect (stops auto-reconnect, use 'connect' to reconnect)
  send <hex>                Send data frame (hex bytes, e.g. "send 01 02 FF")
  ack                       Send ACK to last NID
  rssi                      Send RSSI query to last NID
  telemetry                 Send telemetry test frame (X=0 Y=0 Btn=0)
  autoack [on|off]          Toggle auto ACK
  nid [hex]                 Set/get NID filter (0 = accept all)
  prefix [hex]              Set/get gateway prefix NID
  autorc [on|off]           Toggle auto reconnect (default: ON)
  retry [seconds]           Set/get reconnect interval (1-60s, default: 3)
  rmax [n]                  Set/get max reconnect attempts (0=unlimited, default: 0)
  status                    Show connection status and stats

UDP Config Commands:
  search                    Search for LoRa gateway devices
  getnet                    Get network parameters (IP/Mask/GW)
  at <cmd>                  Send AT command via UDP (e.g. "at AT+VER?")
  ver                       Query firmware version
  gwid                      Query gateway ID
  csq                       Query signal strength
  dhcp [on|off]             DHCP on/off
  ip [addr]                 Set/query gateway IP
  mask [addr]               Set/query subnet mask
  gw [addr]                 Set/query gateway address
  option [0-4]              Set/query OPTION (0=socket 1=serial 2=mqtt 3=ali 4=usr)
  nwmode [0-1]              Set/query NWMODE (0=透传 1=组网)
  ttmode [0-1]              Set/query TTMODE (0=广播透传 1=指定节点)
  wmode [0-2]               Set/query WMODE (0=广播透传 1=指定节点 2=主动上报)
  upwid [on|off]            Set/query UPWID
  ch <n> [freq]             Set/query channel (n=1-2, freq=4100-5100)
  spd <n> [val]             Set/query speed (n=1-2, val=4-11)
  pwr <n> [val]             Set/query power (n=1-2, val=24-30)
  rl [1-4]                  Set RSSI level for auto-response

General:
  help                      Show this help
  quit                      Disconnect and exit
"""


def parse_hex(s: str) -> bytes:
    parts = s.strip().split()
    return bytes(int(x, 16) for x in parts)


# ── 主程序 ──

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <ip> <port> [--nid HEX] [--no-auto-ack]")
        print(f"       {sys.argv[0]} --no-connect [--nid HEX]  (start without auto-connect)")
        print(f"Example: {sys.argv[0]} 192.168.2.100 1234 --nid 00000001")
        sys.exit(1)

    ip = sys.argv[1]
    port = 0
    skip_connect = False

    if sys.argv[1] == "--no-connect":
        skip_connect = True
    else:
        port = int(sys.argv[2])

    nid_filter = 0
    auto_ack = True

    i = 3 if not skip_connect else 2
    while i < len(sys.argv):
        if sys.argv[i] == "--nid" and i + 1 < len(sys.argv):
            nid_filter = int(sys.argv[i + 1], 16)
            i += 2
        elif sys.argv[i] == "--no-auto-ack":
            auto_ack = False
            i += 1
        else:
            i += 1

    st = LoRaState()
    st.nid_filter = nid_filter
    st.auto_ack = auto_ack
    st.gateway_nid = nid_filter if nid_filter else 0x00000001
    st.last_nid = nid_filter

    if not skip_connect:
        print(f"Connecting to {ip}:{port} ...")
        if not tcp_connect(st, ip, port):
            print("Starting in disconnected mode. Use 'connect <ip> <port>' to connect.")
    else:
        print("Starting in disconnected mode.")

    # 启动自动重连线程
    st.reconnect_thread = threading.Thread(
        target=reconnect_watcher, args=(st,), daemon=True
    )
    st.reconnect_thread.start()

    print("Type 'help' for commands.\n")

    try:
        while not st.quit_event.is_set():
            try:
                line = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                break

            if not line:
                continue

            parts = line.split(None, 1)
            cmd = parts[0].lower()
            arg = parts[1] if len(parts) > 1 else ""

            # ── TCP 命令 ──
            if cmd in ("q", "quit", "exit"):
                break

            elif cmd in ("h", "help"):
                print(HELP_TEXT)

            elif cmd == "connect":
                if st.connected:
                    print("  Already connected. Use 'disconnect' first.")
                    continue
                parts2 = arg.split()
                if len(parts2) < 2:
                    print("  Usage: connect <ip> <port>")
                    continue
                try:
                    c_ip = parts2[0]
                    c_port = int(parts2[1])
                except ValueError:
                    print("  Invalid port number")
                    continue
                print(f"  Connecting to {c_ip}:{c_port} ...")
                tcp_connect(st, c_ip, c_port)

            elif cmd == "disconnect":
                tcp_disconnect(st)

            elif cmd in ("s", "send"):
                try:
                    data = parse_hex(arg)
                    target = st.last_nid or st.gateway_nid
                    tcp_send_frame(st, target, data)
                    print(f"  >> TX Data [{target:08X}] {len(data)}B: {data.hex(' ').upper()}")
                except Exception as e:
                    print(f"  Error: {e}")

            elif cmd in ("a", "ack"):
                target = st.last_nid or st.gateway_nid
                send_ack_tcp(st, target)

            elif cmd in ("r", "rssi"):
                target = st.last_nid or st.gateway_nid
                tcp_send_frame(st, target, bytes([DATA_RSSI]))
                print(f"  >> TX RSSI Request [{target:08X}]")

            elif cmd in ("t", "telemetry"):
                target = st.last_nid or st.gateway_nid
                x = struct.pack(">h", 0)
                y = struct.pack(">h", 0)
                telemetry = bytes([DATA_HANDLER]) + x + y + bytes([0x00, 0xFF, 0xFF, 0xFF])
                tcp_send_frame(st, target, telemetry)
                print(f"  >> TX Telemetry [{target:08X}]")

            elif cmd == "autoack":
                if arg.lower() == "off":
                    st.auto_ack = False
                elif arg.lower() == "on":
                    st.auto_ack = True
                else:
                    st.auto_ack = not st.auto_ack
                print(f"  Auto ACK: {'ON' if st.auto_ack else 'OFF'}")

            elif cmd in ("n", "nid"):
                if arg:
                    try:
                        st.nid_filter = int(arg, 16)
                        st.last_nid = st.nid_filter
                        print(f"  NID filter set to {st.nid_filter:08X}")
                    except ValueError:
                        print("  Invalid hex")
                else:
                    print(f"  NID filter: {st.nid_filter:08X}")

            elif cmd in ("p", "prefix"):
                if arg:
                    try:
                        st.gateway_nid = int(arg, 16)
                        print(f"  Gateway prefix set to {st.gateway_nid:08X}")
                    except ValueError:
                        print("  Invalid hex")
                else:
                    print(f"  Gateway prefix: {st.gateway_nid:08X}")

            elif cmd == "rl":
                if arg and arg.isdigit():
                    st.rssi_level = max(1, min(4, int(arg)))
                print(f"  RSSI level for auto-response: {st.rssi_level}")

            elif cmd == "status":
                conn = "Connected" if st.connected else "Disconnected"
                ar = "ON" if st.auto_reconnect else "OFF"
                print(f"  Status: {conn}")
                print(
                    f"  Stats: RX={st.stats['rx']} TX={st.stats['tx']} "
                    f"TX_ACK={st.stats['tx_ack']} ERR={st.stats['err']}"
                )
                print(f"  NID filter: {st.nid_filter:08X}  Gateway prefix: {st.gateway_nid:08X}")
                print(f"  Last NID: {st.last_nid:08X}  Auto ACK: {'ON' if st.auto_ack else 'OFF'}")
                print(f"  Auto reconnect: {ar}  Interval: {st.reconnect_interval}s  "
                      f"Max: {'unlimited' if st.reconnect_max == 0 else st.reconnect_max}")
                if st.last_ip:
                    print(f"  Target: {st.last_ip}:{st.last_port}")

            elif cmd in ("autorc", "autoreconnect"):
                if arg.lower() == "off":
                    st.auto_reconnect = False
                elif arg.lower() == "on":
                    st.auto_reconnect = True
                    st.manual_disconnect = False  # 重新开启时清除手动断开标记
                else:
                    st.auto_reconnect = not st.auto_reconnect
                    if st.auto_reconnect:
                        st.manual_disconnect = False
                print(f"  Auto reconnect: {'ON' if st.auto_reconnect else 'OFF'}")

            elif cmd == "retry":
                if arg:
                    try:
                        st.reconnect_interval = max(1, min(60, int(arg)))
                    except ValueError:
                        print("  Invalid value (1-60 seconds)")
                print(f"  Reconnect interval: {st.reconnect_interval}s")

            elif cmd == "rmax":
                if arg:
                    try:
                        st.reconnect_max = max(0, int(arg))
                    except ValueError:
                        print("  Invalid value (0=unlimited)")
                print(f"  Reconnect max attempts: {'unlimited' if st.reconnect_max == 0 else st.reconnect_max}")

            # ── UDP 命令 ──
            elif cmd == "search":
                cmd_search(st)

            elif cmd == "getnet":
                cmd_getnet(st)

            elif cmd == "at":
                cmd_send_at(st, arg)

            elif cmd == "ver":
                cmd_send_at(st, "AT+VER?\r\n")

            elif cmd == "gwid":
                cmd_send_at(st, "AT+GWID?\r\n")

            elif cmd == "csq":
                cmd_send_at(st, "AT+CSQ?\r\n")

            elif cmd == "dhcp":
                if arg.lower() == "on":
                    cmd_send_at(st, "AT+DHCP=ON\r\n")
                elif arg.lower() == "off":
                    cmd_send_at(st, "AT+DHCP=OFF\r\n")
                else:
                    cmd_send_at(st, "AT+DHCP?\r\n")

            elif cmd == "ip":
                if arg:
                    cmd_send_at(st, f"AT+GWIP={arg}\r\n")
                else:
                    cmd_send_at(st, "AT+GWIP?\r\n")

            elif cmd == "mask":
                if arg:
                    cmd_send_at(st, f"AT+MASK={arg}\r\n")
                else:
                    cmd_send_at(st, "AT+MASK?\r\n")

            elif cmd in ("gateway", "router"):
                if arg:
                    cmd_send_at(st, f"AT+GW={arg}\r\n")
                else:
                    cmd_send_at(st, "AT+GW?\r\n")

            elif cmd == "option":
                if arg and arg.isdigit():
                    cmd_send_at(st, f"AT+OPTION={arg}\r\n")
                else:
                    cmd_send_at(st, "AT+OPTION?\r\n")

            elif cmd == "nwmode":
                if arg and arg.isdigit():
                    cmd_send_at(st, f"AT+NWMODE={arg}\r\n")
                else:
                    cmd_send_at(st, "AT+NWMODE?\r\n")

            elif cmd == "ttmode":
                if arg and arg.isdigit():
                    cmd_send_at(st, f"AT+TTMODE={arg}\r\n")
                else:
                    cmd_send_at(st, "AT+TTMODE?\r\n")

            elif cmd == "wmode":
                if arg and arg.isdigit():
                    cmd_send_at(st, f"AT+WMODE={arg}\r\n")
                else:
                    cmd_send_at(st, "AT+WMODE?\r\n")

            elif cmd == "upwid":
                if arg.lower() == "on":
                    cmd_send_at(st, "AT+UPWID=ON\r\n")
                elif arg.lower() == "off":
                    cmd_send_at(st, "AT+UPWID=OFF\r\n")
                else:
                    cmd_send_at(st, "AT+UPWID?\r\n")

            elif cmd == "ch":
                parts2 = arg.split(None, 1)
                if len(parts2) >= 1 and parts2[0].isdigit():
                    n = int(parts2[0])
                    if len(parts2) >= 2:
                        cmd_send_at(st, f"AT+CH{n}={parts2[1]}\r\n")
                    else:
                        cmd_send_at(st, f"AT+CH{n}?\r\n")
                else:
                    print("  Usage: ch <n> [freq]")

            elif cmd == "spd":
                parts2 = arg.split(None, 1)
                if len(parts2) >= 1 and parts2[0].isdigit():
                    n = int(parts2[0])
                    if len(parts2) >= 2:
                        cmd_send_at(st, f"AT+SPD{n}={parts2[1]}\r\n")
                    else:
                        cmd_send_at(st, f"AT+SPD{n}?\r\n")
                else:
                    print("  Usage: spd <n> [val]")

            elif cmd == "pwr":
                parts2 = arg.split(None, 1)
                if len(parts2) >= 1 and parts2[0].isdigit():
                    n = int(parts2[0])
                    if len(parts2) >= 2:
                        cmd_send_at(st, f"AT+PWR{n}={parts2[1]}\r\n")
                    else:
                        cmd_send_at(st, f"AT+PWR{n}?\r\n")
                else:
                    print("  Usage: pwr <n> [val]")

            else:
                print(f"  Unknown command: {cmd}. Type 'help' for commands.")

    except Exception:
        pass

    st.quit_event.set()
    st.connected = False
    if st.tcp_sock:
        try:
            st.tcp_sock.close()
        except Exception:
            pass
    if st.recv_thread and st.recv_thread.is_alive():
        st.recv_thread.join(timeout=2)
    print(
        f"\n  Stats: RX={st.stats['rx']} TX={st.stats['tx']} "
        f"TX_ACK={st.stats['tx_ack']} ERR={st.stats['err']}"
    )
    print("Bye.")


if __name__ == "__main__":
    main()
