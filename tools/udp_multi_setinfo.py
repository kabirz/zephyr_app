from socket import *
import json
import struct
import time

MULTICAST_GROUP = '224.0.0.1'
address = (MULTICAST_GROUP, 9002)

# socket
s = socket(AF_INET, SOCK_DGRAM)

# set multicast info
s.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)
mreq = struct.pack("4sl", inet_aton(MULTICAST_GROUP), INADDR_ANY)
s.setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq)

# msgs
cfg_msgs = {
   'set_device_info' : {
        # 'ip': '192.168.12.13',
        # 'slave_id': 20,
        # 'rs485_bps': 9600,
        'timestamp': int(time.time()),
   }
}

# send msgs
s.sendto(json.dumps(cfg_msgs).encode(), address)

# rece msgs
data, a = s.recvfrom(1024)
print(f'[recv from {a}]:')
print(json.loads(data.decode()))

s.close()
