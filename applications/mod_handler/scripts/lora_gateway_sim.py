#!/usr/bin/env python3
# pyright: reportMissingTypeArgument=false, reportUnknownParameterType=false, reportUnknownMemberType=false, reportUnknownVariableType=false, reportAny=false, reportUnannotatedClassAttribute=false, reportUnusedParameter=false, reportUnusedCallResult=false, reportUnusedImport=false, reportUnknownArgumentType=false, reportImplicitStringConcatenation=false
"""
LoRa Gateway Simulator — TCP + UDP

模拟 USR-LG210-L 网关 + 远端 LoRa 节点，用于测试 lora_tcp.py 工具。

TCP Server:
  - 接受工具连接, 解析收发帧
  - 响应 RSSI/HANDLER/TEST 帧
  - 支持手动或自动发送模拟遥测/扫描仪数据

UDP Server:
  - 监听 port 1566, 处理 SEARCH/GETPARA/AT 指令
  - 模拟设备发现、网络参数查询、AT 响应

Usage:
  python lora_gateway_sim.py [--tcp-port PORT] [--nid HEX]

命令行参数:
  --tcp-port PORT   TCP 监听端口 (默认 1234)
  --udp-port PORT   UDP 监听端口 (默认 1566)
  --nid HEX         模拟节点 NID (默认 00000001)
  --gwid HEX        模拟网关 GWID (默认 00000005)
"""

import socket
import struct
import sys
import threading
import time
import select
import json
import argparse
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

FRAME_HDR = b'\xAA\x55'
FRAME_FTR = b'\r\n'

DATA_HANDLER = 0x01
DATA_TEST = 0x02
DATA_RSSI = 0x03

TYPE_NAMES = {
    DATA_HANDLER: "HANDLER",
    DATA_TEST: "TEST",
    DATA_RSSI: "RSSI",
}


# ── CRC16-CCITT ──

def crc16_ccitt(seed: int, data: bytes) -> int:
    crc = seed
    for b in data:
        e = (crc ^ b) & 0xFF
        f = (e ^ (e << 4)) & 0xFF
        crc = ((crc >> 8) ^ (f << 8) ^ (f << 3) ^ (f >> 4)) & 0xFFFF
    return crc


# ── 帧构建/解析 ──

def build_frame(nid: int, payload: bytes = b"") -> bytes:
    """构建统一帧: [NID 4B][Length 2B][Data][CRC 2B]"""
    nid_b = struct.pack(">I", nid)
    len_b = struct.pack(">H", len(payload))
    body = nid_b + len_b + payload
    crc = struct.pack(">H", crc16_ccitt(0, body))
    return body + crc


def build_rx_packet(nid: int, payload: bytes = b"") -> bytes:
    """构建 RX 帧 (网关→工具): [0xAA][0x55][统一帧][\r\n]"""
    return FRAME_HDR + build_frame(nid, payload) + FRAME_FTR


def parse_tool_frames(buf: bytes):
    """解析工具发送的帧: [NID 4B][0xAA 0x55][统一帧][\r\n]

    返回 (frames_list, remaining_bytes)
    frame = {"nid": int, "data_len": int, "payload": bytes}
    """
    frames = []
    pos = 0

    while pos < len(buf):
        idx = buf.find(FRAME_HDR, pos)
        if idx < 0:
            break

        if idx < FRAME_NID_SIZE:
            break

        tail = buf.find(FRAME_FTR, idx + 2)
        if tail < 0:
            break

        content = buf[idx + 2 : tail]
        frame_end = tail + len(FRAME_FTR)

        if len(content) >= FRAME_OVERHEAD:
            nid = struct.unpack(">I", content[0:4])[0]
            data_len = struct.unpack(">H", content[4:6])[0]
            total = FRAME_HEADER_SIZE + data_len + FRAME_CRC_SIZE

            if total <= 2048 and len(content) >= total:
                calc_crc = crc16_ccitt(0, content[: FRAME_HEADER_SIZE + data_len])
                rx_crc = struct.unpack(
                    ">H", content[FRAME_HEADER_SIZE + data_len : total]
                )[0]

                if calc_crc == rx_crc:
                    frames.append({
                        "nid": nid,
                        "data_len": data_len,
                        "payload": content[
                            FRAME_HEADER_SIZE : FRAME_HEADER_SIZE + data_len
                        ],
                    })

        pos = frame_end

    return frames, buf[pos:]


# ── UDP 响应构建 ──

def udp_wrap(data: dict) -> bytes:
    json_str = json.dumps(data, separators=(",", ":"))
    return f"USR1566{json_str}USR1566".encode()


def _is_tun_ip(ip: str) -> bool:
    """Check if IP belongs to a VPN/TUN interface"""
    parts = ip.split(".")
    if len(parts) != 4:
        return False
    try:
        a, b = int(parts[0]), int(parts[1])
    except ValueError:
        return False
    if a == 198 and b == 18:
        return True  # Clash TUN
    if a == 172 and 16 <= b <= 31:
        return True  # Docker / VPN
    if a == 100 and 64 <= b <= 127:
        return True  # CGNAT / Tailscale
    return False


def _same_subnet(ip1: str, ip2: str, prefix_len: int = 24) -> bool:
    """Check if two IPs are on the same /24 subnet"""
    p1 = ip1.split(".")
    p2 = ip2.split(".")
    if len(p1) != 4 or len(p2) != 4:
        return False
    if prefix_len == 24:
        return p1[0] == p2[0] and p1[1] == p2[1] and p1[2] == p2[2]
    return False


# ── 模拟器配置 ──

class SimConfig:
    def __init__(self):
        self.nid = 0x00000001
        self.gwid = 0x00000005
        self.mac = "D4AD20ED63C4"
        self.dev_name = "USR-LG210-L"
        self.sw_ver = "V4.1.7"
        self._ip = "127.0.0.1"
        self.mask = "255.255.0.0"
        self.gw = "127.0.0.1"
        self.dhcp = "ON"
        self.option = 0
        self.nwmode = 0
        self.ttmode = 0
        self.wmode = 0
        self.upwid = "OFF"
        self.socken = "ON,OFF"
        self.socka = "TCPC,192.168.1.100,1883,1234"
        self.ch = {1: 4700, 2: 4800}
        self.spd = {1: 7, 2: 7}
        self.pwr = {1: 30, 2: 30}
        self.rssi_snr = 12
        self.rssi_val = -65

    def set_interface_ip(self, ip: str):
        """Set the response IP to the given interface IP and derive GW"""
        self._ip = ip
        parts = ip.split(".")
        if len(parts) == 4:
            self.gw = f"{parts[0]}.{parts[1]}.{parts[2]}.1"

    @property
    def ip(self):
        return self._ip


# ── TCP Server ──

class GatewayTCPServer:
    def __init__(self, cfg: SimConfig, port: int):
        self.cfg = cfg
        self.port = port
        self.client_sock = None
        self.running = False
        self.rx_buf = bytearray()
        self.stats = {"rx": 0, "tx": 0, "err": 0}
        self.auto_telemetry = False
        self.auto_interval = 2.0  # 秒
        self.lock = threading.Lock()

    def start(self):
        self.running = True
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", self.port))
        server.listen(1)
        server.settimeout(1.0)
        print(f"  [TCP] Listening on port {self.port}")

        while self.running:
            try:
                client, addr = server.accept()
            except socket.timeout:
                continue
            except Exception:
                break

            print(f"  [TCP] Client connected: {addr}")
            self.client_sock = client
            self.rx_buf.clear()
            self.stats = {"rx": 0, "tx": 0, "err": 0}

            # 启动自动遥测线程
            auto_thread = threading.Thread(
                target=self._auto_telemetry_loop, daemon=True
            )
            auto_thread.start()

            # 处理客户端数据
            try:
                self._handle_client()
            except Exception as e:
                print(f"  [TCP] Error: {e}")

            print("  [TCP] Client disconnected")
            self.client_sock = None

        server.close()

    def _handle_client(self):
        buf = bytearray(4096)
        while self.running and self.client_sock:
            try:
                ready, _, _ = select.select([self.client_sock], [], [], 0.5)
            except Exception:
                break
            if not ready:
                continue

            nbytes = self.client_sock.recv_into(buf, len(buf))
            if nbytes == 0:
                break

            self.rx_buf.extend(buf[:nbytes])
            frames, remaining = parse_tool_frames(bytes(self.rx_buf))
            self.rx_buf = bytearray(remaining)

            for frame in frames:
                self._process_frame(frame)

    def _process_frame(self, frame: dict):
        nid = frame["nid"]
        payload = frame["payload"]
        plen = frame["data_len"]

        self.stats["rx"] += 1
        ts = time.strftime("%H:%M:%S")

        dtype = payload[0]
        body = payload[1:]
        type_name = TYPE_NAMES.get(dtype, f"0x{dtype:02X}")

        if dtype == DATA_HANDLER and len(body) == 8:
            x = struct.unpack(">h", body[0:2])[0] / 10.0
            y = struct.unpack(">h", body[2:4])[0] / 10.0
            btn = "Pressed" if (body[4] & 0x01) == 0 else "Released"
            print(
                f"  [{ts}] RX Telemetry [{nid:08X}] X={x:.1f} Y={y:.1f} Btn={btn}"
            )
            return

        if dtype == DATA_RSSI and len(body) == 0:
            print(f"  [{ts}] RX RSSI Request [{nid:08X}]")
            rssi_val = self._sim_rssi_value()
            resp = bytes([DATA_RSSI]) + struct.pack(">b", rssi_val)
            self._send_to_client(nid, resp)
            print(f"  [{ts}] TX RSSI Response [{nid:08X}] {rssi_val} dBm")
            return

        body_hex = body.hex(" ").upper() if body else ""
        print(f"  [{ts}] RX {type_name} [{nid:08X}] {plen - 1}B: {body_hex}")

    def _send_to_client(self, nid: int, payload: bytes):
        if not self.client_sock:
            return
        pkt = build_rx_packet(nid, payload)
        try:
            self.client_sock.sendall(pkt)
            self.stats["tx"] += 1
        except Exception:
            self.client_sock = None

    def send_telemetry(self, x: int = 0, y: int = 0, btn: int = 1):
        """发送模拟遥测数据 (网关→工具)"""
        data = bytes([DATA_HANDLER])
        data += struct.pack(">h", x)
        data += struct.pack(">h", y)
        data += bytes([0x00 if btn else 0x01, 0xFF, 0xFF, 0xFF])
        self._send_to_client(self.cfg.nid, data)
        ts = time.strftime("%H:%M:%S")
        print(
            f"  [{ts}] TX Telemetry [{self.cfg.nid:08X}] X={x / 10:.1f} Y={y / 10:.1f}"
        )

    def send_scanner(self, can_id: int, can_data: bytes):
        """发送模拟扫描仪数据 (网关→工具)"""
        data = bytes([DATA_HANDLER]) + struct.pack(">H", can_id) + can_data
        self._send_to_client(self.cfg.nid, data)
        ts = time.strftime("%H:%M:%S")
        print(
            f"  [{ts}] TX Scanner [{self.cfg.nid:08X}] CAN=0x{can_id:03X} data={can_data.hex(' ').upper()}"
        )

    def send_test(self, data: bytes):
        """发送测试数据"""
        payload = bytes([DATA_TEST]) + data
        self._send_to_client(self.cfg.nid, payload)
        ts = time.strftime("%H:%M:%S")
        print(f"  [{ts}] TX Test [{self.cfg.nid:08X}] {data.hex(' ').upper()}")

    def send_rssi_request(self):
        """发送 RSSI 请求 (模拟远端节点请求)"""
        self._send_to_client(self.cfg.nid, bytes([DATA_RSSI]))
        ts = time.strftime("%H:%M:%S")
        print(f"  [{ts}] TX RSSI Request [{self.cfg.nid:08X}]")

    def _sim_rssi_value(self) -> int:
        """模拟 RSSI 值 (dBm)"""
        return self.cfg.rssi_val

    def _auto_telemetry_loop(self):
        """自动遥测循环"""
        import random
        while self.running and self.client_sock:
            if self.auto_telemetry:
                x = random.randint(-50, 50)
                y = random.randint(-50, 50)
                btn = random.choice([0, 1])
                self.send_telemetry(x, y, btn)
            time.sleep(self.auto_interval)


# ── UDP Server ──

class GatewayUDPServer:
    def __init__(self, cfg: SimConfig, port: int):
        self.cfg = cfg
        self.port = port
        self.running = False
        self.local_ips = []  # cache of local interface IPs

    def _get_local_ips(self):
        """Get all non-loopback, non-TUN local IPv4 addresses (cross-platform)"""
        ips = []
        seen = set()
        # Method 1: connect to public DNS to discover primary outbound IP
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            primary = s.getsockname()[0]
            s.close()
            if not primary.startswith("127.") and not _is_tun_ip(primary) and primary not in seen:
                ips.append(primary)
                seen.add(primary)
        except Exception:
            pass
        # Method 2: hostname resolution
        try:
            hostname = socket.gethostname()
            _, _, addrs = socket.gethostbyname_ex(hostname)
            for ip in addrs:
                if not ip.startswith("127") and not _is_tun_ip(ip) and ip not in seen:
                    ips.append(ip)
                    seen.add(ip)
        except Exception:
            pass
        # Method 3: Windows ipconfig
        if not ips and sys.platform == "win32":
            try:
                import subprocess
                out = subprocess.check_output(["ipconfig"], text=True, encoding="gbk", errors="replace")
                for line in out.splitlines():
                    line = line.strip()
                    if line.startswith("IPv4") and ":" in line:
                        ip = line.split(":")[-1].strip().rstrip(")")
                        if "%" in ip:
                            ip = ip.split("%")[0]
                        if ip and not ip.startswith("127") and not _is_tun_ip(ip) and ip not in seen:
                            try:
                                socket.inet_aton(ip)
                                ips.append(ip)
                                seen.add(ip)
                            except socket.error:
                                pass
            except Exception:
                pass
        return ips

    def _get_iface_info(self, target_ip: str):
        """Find the IP and GW for the interface that owns target_ip"""
        # Simple heuristic: find the matching local IP and derive GW
        for ip in self.local_ips:
            if ip == target_ip:
                # Derive GW by replacing last octet with .1
                parts = ip.split(".")
                gw = f"{parts[0]}.{parts[1]}.{parts[2]}.1"
                return ip, gw
        # Fallback
        if self.local_ips:
            ip = self.local_ips[0]
            parts = ip.split(".")
            return ip, f"{parts[0]}.{parts[1]}.{parts[2]}.1"
        return "127.0.0.1", "127.0.0.1"

    def start(self):
        self.running = True
        self.local_ips = self._get_local_ips()

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        if sys.platform == "win32":
            # Windows: IP_RECVDSTADDR gives us the destination IP
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_RECVDSTADDR, 1)
        else:
            # Linux/macOS: IP_PKTINFO gives us the destination IP + interface index
            try:
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_PKTINFO, 1)
            except OSError:
                pass
        sock.bind(("0.0.0.0", self.port))
        sock.settimeout(1.0)
        print(f"  [UDP] Listening on port {self.port} (interfaces: {', '.join(self.local_ips)})")

        buf = bytearray(4096)
        cmsg_buf = bytearray(1024)
        has_recvmsg = hasattr(sock, 'recvmsg_into')

        while self.running:
            try:
                if has_recvmsg:
                    # recvmsg_into is Python 3.9+, not available on Windows
                    _fn = getattr(sock, "recvmsg_into", None)
                    if _fn is not None:
                        nbytes, addr, _flags, cmsg = _fn(buf, len(buf), cmsg_buf, len(cmsg_buf))
                    else:
                        nbytes, addr = sock.recvfrom_into(buf, len(buf))
                        cmsg = None
                else:
                    # Windows: use getsockopt IP_RECVDSTADDR to get destination IP
                    nbytes, addr = sock.recvfrom_into(buf, len(buf))
                    cmsg = None
            except (socket.timeout, OSError):
                continue
            except Exception:
                break

            # Determine which local interface received this packet
            dst_ip = None
            if cmsg:
                # Linux/macOS: parse IP_PKTINFO cmsg
                for c_level, c_type, c_data in cmsg:
                    if c_level == socket.IPPROTO_IP and c_type == socket.IP_PKTINFO:
                        dst_ip = socket.inet_ntoa(c_data[4:8])
                        break

            # Windows / fallback: find local IP on the same subnet as source
            if not dst_ip and addr:
                src_ip = addr[0]
                for local_ip in self.local_ips:
                    if _same_subnet(src_ip, local_ip):
                        dst_ip = local_ip
                        break
                if not dst_ip and self.local_ips:
                    dst_ip = self.local_ips[0]

            # If we got a valid destination, use that interface
            if dst_ip and not dst_ip.startswith("127") and not _is_tun_ip(dst_ip):
                self.cfg.set_interface_ip(dst_ip)

            raw = bytes(buf[:nbytes]).decode("utf-8", errors="replace")
            ts = time.strftime("%H:%M:%S")
            iface_tag = f"[{dst_ip}]" if dst_ip else ""
            print(f"  [{ts}] [UDP] RX {iface_tag} from {addr[0]}:{addr[1]}: {raw.strip()[:80]}")

            response = self._handle_udp(raw)
            if response:
                sock.sendto(response, addr)
                print(f"  [{ts}] [UDP] TX -> {addr[0]}:{addr[1]}: response sent")

        sock.close()

    def _handle_udp(self, raw: str) -> bytes | None:
        # 提取 JSON
        start = raw.find("{")
        end = raw.rfind("}")
        if start < 0 or end <= start:
            return None

        try:
            root = json.loads(raw[start : end + 1])
        except json.JSONDecodeError:
            return None

        msg = root.get("MSG", "")

        if msg == "SEARCH":
            return self._handle_search(root)

        if msg in ("GETPARA", "SETPARA"):
            cmd_type = root.get("TYPE", "")
            ack_msg = "ACK-GETPARA" if msg == "GETPARA" else "ACK-SETPARA"
            if cmd_type == "JSON":
                cmd_val = root.get("CMD", "")
                if cmd_val == "NETDEV":
                    return self._handle_getnet(root, ack_msg)
            elif cmd_type == "AT":
                at_cmd = root.get("CMD", "")
                return self._handle_at(root, at_cmd, ack_msg)

        return None

    def _handle_search(self, req: dict) -> bytes:
        return udp_wrap({
            "VER": "1.0",
            "MSG": "ACK-SEARCH",
            "MAC": self.cfg.mac,
            "DEV": self.cfg.dev_name,
            "SVER": self.cfg.sw_ver,
            "TYPE": "LORA",
        })

    def _handle_getnet(self, req: dict, ack_msg: str = "ACK-GETPARA") -> bytes:
        return udp_wrap({
            "VER": "1.0",
            "MSG": ack_msg,
            "CMD": {
                "IP": self.cfg.ip,
                "SM": self.cfg.mask,
                "GW": self.cfg.gw,
            },
        })

    def _handle_at(self, req: dict, at_cmd: str, ack_msg: str = "ACK-GETPARA") -> bytes:
        at_cmd = at_cmd.strip()
        resp = self._simulate_at(at_cmd)
        return udp_wrap({
            "VER": "1.0",
            "MSG": ack_msg,
            "CMD": resp,
        })

    def _simulate_at(self, cmd: str) -> str:
        """模拟 AT 指令响应"""
        cmd_upper = cmd.upper().rstrip("\r\n")

        if cmd_upper == "AT+VER?":
            return f"\r\n+VER:{self.cfg.sw_ver}\r\n\r\nOK\r\n"

        if cmd_upper == "AT+GWID?":
            return f"\r\n+GWID:{self.cfg.gwid:08X}\r\n\r\nOK\r\n"

        if cmd_upper == "AT+CSQ?":
            return "\r\n+CSQ:4,18\r\n\r\nOK\r\n"

        if cmd_upper == "AT+DHCP?":
            return f"\r\n+DHCP:{self.cfg.dhcp}\r\n\r\nOK\r\n"

        if cmd_upper.startswith("AT+DHCP="):
            self.cfg.dhcp = cmd_upper.split("=")[1].strip()
            return "\r\nOK\r\n"

        if cmd_upper == "AT+GWIP?":
            return f"\r\n+GWIP:{self.cfg.ip}\r\n\r\nOK\r\n"

        if cmd_upper.startswith("AT+GWIP="):
            self.cfg.set_interface_ip(cmd_upper.split("=")[1].strip())
            return "\r\nOK\r\n"

        if cmd_upper == "AT+MASK?":
            return f"\r\n+MASK:{self.cfg.mask}\r\n\r\nOK\r\n"

        if cmd_upper.startswith("AT+MASK="):
            self.cfg.mask = cmd_upper.split("=")[1].strip()
            return "\r\nOK\r\n"

        if cmd_upper == "AT+GW?":
            return f"\r\n+GW:{self.cfg.gw}\r\n\r\nOK\r\n"

        if cmd_upper.startswith("AT+GW="):
            self.cfg.gw = cmd_upper.split("=")[1].strip()
            return "\r\nOK\r\n"

        if cmd_upper == "AT+OPTION?":
            return f"\r\n+OPTION:{self.cfg.option}\r\n\r\nOK\r\n"

        if cmd_upper.startswith("AT+OPTION="):
            self.cfg.option = int(cmd_upper.split("=")[1].strip())
            return "\r\nOK\r\n"

        # NWMODE? / NWMODE=<0|1> — 组网模式
        if cmd_upper == "AT+NWMODE?":
            return f"\r\n+NWMODE:{self.cfg.nwmode}\r\n\r\nOK\r\n"
        if cmd_upper.startswith("AT+NWMODE="):
            self.cfg.nwmode = int(cmd_upper.split("=")[1].strip())
            return "\r\n+NWMODE:OK\r\n"

        # TTMODE? / TTMODE=<0|1> — 不组网工作模式
        if cmd_upper == "AT+TTMODE?":
            return f"\r\n+TTMODE:{self.cfg.ttmode}\r\n\r\nOK\r\n"
        if cmd_upper.startswith("AT+TTMODE="):
            self.cfg.ttmode = int(cmd_upper.split("=")[1].strip())
            return "\r\n+TTMODE:OK\r\n"

        # WMODE? / WMODE=<0|1> — 组网工作模式
        if cmd_upper == "AT+WMODE?":
            return f"\r\n+WMODE:{self.cfg.wmode}\r\n\r\nOK\r\n"
        if cmd_upper.startswith("AT+WMODE="):
            self.cfg.wmode = int(cmd_upper.split("=")[1].strip())
            return "\r\n+WMODE:OK\r\n"

        if cmd_upper == "AT+UPWID?":
            return f"\r\n+UPWID:{self.cfg.upwid}\r\n\r\nOK\r\n"
        if cmd_upper.startswith("AT+UPWID="):
            val = cmd_upper.split("=")[1].strip().upper()
            if val in ("ON", "OFF"):
                self.cfg.upwid = val
            return f"\r\n+UPWID:{self.cfg.upwid}\r\n"

        # SOCKEN?<status>,<status> / SOCKEN=<status>,<status>
        if cmd_upper == "AT+SOCKEN?":
            return f"\r\n+SOCKEN:{self.cfg.socken}\r\n\r\nOK\r\n"
        if cmd_upper.startswith("AT+SOCKEN="):
            self.cfg.socken = cmd_upper.split("=", 1)[1].strip()
            return "\r\n+SOCKEN:OK\r\n"

        # SOCKA?<mode>,<ip>,<rport>,<lport> / SOCKA=<mode>,<ip>,<rport>,<lport>
        if cmd_upper == "AT+SOCKA?":
            return f"\r\n+SOCKA:{self.cfg.socka}\r\n\r\nOK\r\n"
        if cmd_upper.startswith("AT+SOCKA="):
            self.cfg.socka = cmd_upper.split("=", 1)[1].strip()
            return "\r\n+SOCKA:OK\r\n"

        # CH(n)? / CH(n)=<freq> — 查询/设置通道n的信道频率 (4100-5100)
        if cmd_upper.startswith("AT+CH") and len(cmd_upper) > 5:
            rest = cmd_upper[5:]
            if "=" in rest:
                n = int(rest.split("=")[0])
                val = rest.split("=")[1].strip()
                self.cfg.ch[n] = int(val)
                return f"\r\n+CH{n}:OK\r\n"
            if rest.endswith("?"):
                n = int(rest[:-1])
                val = self.cfg.ch.get(n, 4700)
                return f"\r\n+CH{n}:{val}\r\n\r\nOK\r\n"

        # SPD(n)? / SPD(n)=<speed> — 查询/设置通道n的速率 (4-11)
        if cmd_upper.startswith("AT+SPD") and len(cmd_upper) > 6:
            rest = cmd_upper[6:]
            if "=" in rest:
                n = int(rest.split("=")[0])
                val = rest.split("=")[1].strip()
                self.cfg.spd[n] = int(val)
                return f"\r\n+SPD{n}:OK\r\n"
            if rest.endswith("?"):
                n = int(rest[:-1])
                val = self.cfg.spd.get(n, 7)
                return f"\r\n+SPD{n}:{val}\r\n\r\nOK\r\n"

        # PWR(n)? / PWR(n)=<power> — 查询/设置通道n的功率 (24-30)
        if cmd_upper.startswith("AT+PWR") and len(cmd_upper) > 6:
            rest = cmd_upper[6:]
            if "=" in rest:
                n = int(rest.split("=")[0])
                val = rest.split("=")[1].strip()
                self.cfg.pwr[n] = int(val)
                return f"\r\n+PWR{n}:OK\r\n"
            if rest.endswith("?"):
                n = int(rest[:-1])
                val = self.cfg.pwr.get(n, 30)
                return f"\r\n+PWR{n}:{val}\r\n\r\nOK\r\n"

        # NINFO?
        if cmd_upper == "AT+NINFO?":
            nid_str = f"{self.cfg.nid >> 16:03X},{self.cfg.nid & 0xFFFF:04X}"
            return (
                f"\r\n+NINFO:{nid_str},1,"
                f"+{self.cfg.rssi_snr:03d},"
                f"+{abs(self.cfg.rssi_val):03d},"
                f"{self.cfg.gwid:08X},00000000,"
                f"1,2026/04/21-12:00:00,0000000000,000\r\n"
                f"\r\nOK\r\n"
            )

        # NID?
        if cmd_upper == "AT+NID?":
            return f"\r\n+NID:{self.cfg.nid:08X}\r\n\r\nOK\r\n"

        # GWID=
        if cmd_upper.startswith("AT+GWID="):
            self.cfg.gwid = int(cmd_upper.split("=")[1].strip(), 16)
            return "\r\nOK\r\n"

        # NID=
        if cmd_upper.startswith("AT+NID="):
            self.cfg.nid = int(cmd_upper.split("=")[1].strip(), 16)
            return "\r\nOK\r\n"

        # 默认响应
        return "\r\nOK\r\n"


# ── 帮助 ──

HELP_TEXT = """
Simulator Commands:
  telemetry [x] [y] [btn]     Send simulated telemetry (default: 0 0 1)
  scanner <can_id> <hex_data>  Send simulated scanner data
    e.g. scanner 263 01 02 03 04 05 06 07 08
  test <hex_data>              Send test data
  rssi                         Send RSSI request to tool
  auto [on|off] [interval]     Toggle auto telemetry (default: 2s)
  nid [hex]                    Set/get simulated NID
  gwid [hex]                   Set/get simulated GWID
  stats                        Show TX/RX/ERR statistics
  help                         Show this help
  quit                         Stop simulator
"""


# ── 主程序 ──

def parse_hex(s: str) -> bytes:
    parts = s.strip().split()
    return bytes(int(x, 16) for x in parts)


def main():
    parser = argparse.ArgumentParser(description="LoRa Gateway Simulator")
    parser.add_argument("--tcp-port", type=int, default=1234, help="TCP port (default: 1234)")
    parser.add_argument("--udp-port", type=int, default=1566, help="UDP port (default: 1566)")
    parser.add_argument("--nid", type=str, default="00000001", help="Node ID hex")
    parser.add_argument("--gwid", type=str, default="00000005", help="Gateway ID hex")
    args = parser.parse_args()

    cfg = SimConfig()
    cfg.nid = int(args.nid, 16)
    cfg.gwid = int(args.gwid, 16)

    print("LoRa Gateway Simulator")
    print(f"  NID={cfg.nid:08X}  GWID={cfg.gwid:08X}")
    print(f"  TCP port={args.tcp_port}  UDP port={args.udp_port}")
    print()

    # 启动 TCP + UDP 服务
    tcp_server = GatewayTCPServer(cfg, args.tcp_port)
    udp_server = GatewayUDPServer(cfg, args.udp_port)

    tcp_thread = threading.Thread(target=tcp_server.start, daemon=True)
    udp_thread = threading.Thread(target=udp_server.start, daemon=True)
    tcp_thread.start()
    udp_thread.start()

    print("  Servers started. Type 'help' for commands.\n")

    try:
        while True:
            try:
                line = input("sim> ").strip()
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
                print(HELP_TEXT)

            elif cmd == "telemetry":
                vals = arg.split() if arg else []
                x = int(vals[0]) if len(vals) > 0 else 0
                y = int(vals[1]) if len(vals) > 1 else 0
                btn = int(vals[2]) if len(vals) > 2 else 1
                tcp_server.send_telemetry(x, y, btn)

            elif cmd == "scanner":
                if not arg:
                    print("  Usage: scanner <can_id_hex> <hex_data>")
                    continue
                parts2 = arg.split(None, 1)
                try:
                    can_id = int(parts2[0], 16)
                    can_data = parse_hex(parts2[1]) if len(parts2) > 1 else b""
                    tcp_server.send_scanner(can_id, can_data)
                except (ValueError, IndexError) as e:
                    print(f"  Error: {e}")

            elif cmd == "test":
                if not arg:
                    print("  Usage: test <hex_data>")
                    continue
                try:
                    tcp_server.send_test(parse_hex(arg))
                except ValueError as e:
                    print(f"  Error: {e}")

            elif cmd == "rssi":
                tcp_server.send_rssi_request()

            elif cmd == "auto":
                parts2 = arg.split() if arg else []
                if parts2 and parts2[0].lower() == "off":
                    tcp_server.auto_telemetry = False
                elif parts2 and parts2[0].lower() == "on":
                    tcp_server.auto_telemetry = True
                    if len(parts2) > 1:
                        tcp_server.auto_interval = float(parts2[1])
                else:
                    tcp_server.auto_telemetry = not tcp_server.auto_telemetry
                state = "ON" if tcp_server.auto_telemetry else "OFF"
                print(f"  Auto telemetry: {state} (interval={tcp_server.auto_interval}s)")

            elif cmd == "nid":
                if arg:
                    try:
                        cfg.nid = int(arg, 16)
                        print(f"  NID set to {cfg.nid:08X}")
                    except ValueError:
                        print("  Invalid hex")
                else:
                    print(f"  NID: {cfg.nid:08X}")

            elif cmd == "gwid":
                if arg:
                    try:
                        cfg.gwid = int(arg, 16)
                        print(f"  GWID set to {cfg.gwid:08X}")
                    except ValueError:
                        print("  Invalid hex")
                else:
                    print(f"  GWID: {cfg.gwid:08X}")

            elif cmd == "stats":
                print(
                    f"  TCP: RX={tcp_server.stats['rx']} TX={tcp_server.stats['tx']} ERR={tcp_server.stats['err']}"
                )
                print(f"  Auto telemetry: {'ON' if tcp_server.auto_telemetry else 'OFF'}")

            else:
                print(f"  Unknown command: {cmd}. Type 'help' for commands.")

    except Exception:
        pass

    tcp_server.running = False
    udp_server.running = False
    print("\nSimulator stopped.")


if __name__ == "__main__":
    main()
