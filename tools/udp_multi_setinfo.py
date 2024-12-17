import argparse
import ipaddress
import struct
import time
from socket import (
    AF_INET,
    IP_ADD_MEMBERSHIP,
    IPPROTO_IP,
    SO_REUSEADDR,
    SOCK_DGRAM,
    SOL_SOCKET,
    inet_aton,
    socket,
)


def valid_ip(address):
    try:
        ipaddress.ip_address(address)
        return address
    except ValueError:
        raise argparse.ArgumentTypeError(f'{address} is not valid ip')


parser = argparse.ArgumentParser(description='udp multicast set device info')
parser.add_argument('--cbor2', action='store_true', help='use cbor2 as data struct')
parser.add_argument('--ip', default='192.168.12.10', type=valid_ip, help='interface ip')
args = parser.parse_args()

MULTICAST_GROUP = '224.0.0.1'
address = (MULTICAST_GROUP, 9002)

# socket
s = socket(AF_INET, SOCK_DGRAM)

# set multicast info
s.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)
mreq = struct.pack('4s4s', inet_aton(MULTICAST_GROUP), inet_aton(args.ip))
s.setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq)

# msgs
cfg_msgs = {
    'set_device_info': {
        # 'ip': [192, 168, 12, 101],
        # 'slave_id': 20,
        # 'rs485_bps': 9600,
        'timestamp': int(time.time()),
    }
}

# send msgs
if args.cbor2:
    import cbor2

    s.sendto(cbor2.dumps(cfg_msgs), address)
else:
    import json

    s.sendto(json.dumps(cfg_msgs).encode(), address)

# recv msgs
data, a = s.recvfrom(1024)
print(f'[recv from {a}]:')
if args.cbor2:
    print(cbor2.loads(data))
else:
    print(json.loads(data.decode()))

s.close()
