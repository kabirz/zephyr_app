from socket import *
import json
import cbor2
import struct

MULTICAST_GROUP = '224.0.0.1'
address = (MULTICAST_GROUP, 9002)
USE_JSON = True

# socket
s = socket(AF_INET, SOCK_DGRAM)

# set multicast info
s.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)
mreq = struct.pack("4sl", inet_aton(MULTICAST_GROUP), INADDR_ANY)
s.setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq)

# msgs
query_msgs = {
    'get_device_info': True,
}

# send msgs
if USE_JSON:
    s.sendto(json.dumps(query_msgs).encode(), address)
else:
    s.sendto(cbor2.dumps(query_msgs), address)

# recv msgs
data, a = s.recvfrom(1024)
print(f'[recv from {a}]:')
if USE_JSON:
    print(json.loads(data.decode()))
else:
    print(cbor2.loads(data))

s.close()
