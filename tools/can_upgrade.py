import can
import argparse
import os
import struct
import sys

if sys.platform.startswith('win'):
    interface, channel = 'pcan', 'PCAN_USBBUS1'
else:
    interface, channel = 'socketcan', 'can0'
parser = argparse.ArgumentParser("can tool")
parser.add_argument('-c' , '--channel', type=str, default=channel, help='signed image firmware file name')
subparser = parser.add_subparsers(dest='command')
flash_parser = subparser.add_parser('flash', help='firmware upgrade')
flash_parser.add_argument('file', help='signed image firmware file name')
version_parser = subparser.add_parser('version', help='firmware upgrade')

FW_CODE_OFFSET = 0
FW_CODE_UPDATE_SUCCESS = 1
FW_CODE_VERSION = 2
FW_CODE_CONFIMR = 3
FW_CODE_FLASH_ERROR = 4
FW_CODE_TRANFER_ERROR = 5

args = parser.parse_args()

bus = can.interface.Bus(interface=interface, channel=args.channel, bitrate=250000)

filters = [
    {"can_id": 0x105, "can_mask": 0x10f, "extended": False},
]
bus.set_filters(filters)

def can_recv(bus: can.BusABC, timeout=5):
    while True:
        rx_frame = bus.recv(timeout)
        if not rx_frame:
            raise BaseException("can receive data timeout")
        if rx_frame.arbitration_id == 0x105:
            return struct.unpack('>2I', rx_frame.data)

def firmware_upgrade(file_name):
    with open(file_name, 'rb') as f:
        total_size = os.path.getsize(args.file)
        data = total_size.to_bytes(4)
        msg = can.Message(arbitration_id=0x101, data=data, is_extended_id=False)
        bus.send(msg)
        f_offset = 0
        code, offset = can_recv(bus)
        if code != FW_CODE_OFFSET and offset != 0:
            raise BaseException(f"flash erase error: code({code}), offset({f_offset}, {offset})")
        while True:
            chunk = f.read(8)
            if not chunk:
                print(1)
                break
            else:
                f_offset += len(chunk)
            msg = can.Message(arbitration_id=0x102, data=chunk, is_extended_id=False)
            bus.send(msg)
            code, offset = can_recv(bus)
            if code == FW_CODE_UPDATE_SUCCESS and offset == f_offset:
                print(2)
                break
            if code != FW_CODE_OFFSET:
                raise BaseException(f"firmware upload error: code({code}), offset({f_offset}, {offset})")

        msg = can.Message(arbitration_id=0x103, data=[1], is_extended_id=False)
        bus.send(msg)
        code, offset = can_recv(bus, timeout=30)
        if code == FW_CODE_CONFIMR and offset == 0x55AA55AA:
            print(f"Image {args.file} upload finished, board will reboot and upgrade, it will take about 90~150s")
        elif code == FW_CODE_TRANFER_ERROR:
            print("Download Failed")


def firmware_version():
    msg = can.Message(arbitration_id=0x104, data=[1], is_extended_id=False)
    bus.send(msg)
    code, version = can_recv(bus)
    if code == FW_CODE_VERSION:
        ver1, ver2, ver3 =  version & 0xff, (version > 8) & 0xff, (version > 16) & 0xff
        print(f"version: v{ver1}.{ver2}.{ver3}")

if __name__ == "__main__":
    print(args)
    if args.command == 'version':
        firmware_version()
    elif args.command == 'flash':
        firmware_upgrade(args.file)

