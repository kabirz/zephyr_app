import canopen
import time
import pathlib

'''
changed zephyr:
drivers/can/can_native_linux.c
CAN_DT_DRIVER_CONFIG_INST_GET(inst, 1000, 1000000)

device
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
sudo ip link property add dev vcan0 altname zcan0
'''

CURRENT = pathlib.Path(__file__).parent.parent
EDS = CURRENT/'objdict/objdict.eds'

NODEID = 1

network = canopen.Network()

network.connect(bustype='socketcan', channel='zcan0')

node = network.add_node(NODEID, EDS.as_posix())

node.nmt.state = 'PRE-OPERATIONAL'
time.sleep(1)

node.nmt.state = 'OPERATIONAL'
time.sleep(1)

node.nmt.wait_for_heartbeat()
print('end')
# network.disconnect()


