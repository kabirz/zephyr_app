import can
import argparse
import os
import struct
import sys
import tqdm

if sys.platform.startswith('win'):
    interface, channel = 'pcan', 'PCAN_USBBUS1'
else:
    interface, channel = 'socketcan', 'can0'
parser = argparse.ArgumentParser("can tool")
parser.add_argument('-c', '--channel', type=str, default=channel, help='signed image firmware file name')
subparser = parser.add_subparsers(dest='command')
flash_parser = subparser.add_parser('flash', help='firmware upgrade')
flash_parser.add_argument('file', help='signed image firmware file name')
flash_parser.add_argument('-t', '--test', action='store_true', help='upgrade only for test, will revert next reboot')
board_parser = subparser.add_parser('board', help='board opt')
board_parser.add_argument('-r', '--reboot', action='store_true', help='reboot board')
board_parser.add_argument('-v', '--version', action='store_true', help='get board version')


FW_CODE_OFFSET = 0
FW_CODE_UPDATE_SUCCESS = 1
FW_CODE_VERSION = 2
FW_CODE_CONFIRM = 3
FW_CODE_FLASH_ERROR = 4
FW_CODE_TRANFER_ERROR = 5

PLATFORM_RX   = 0x101
PLATFORM_TX   = 0x102
FW_DATA_RX    = 0x103

BOARD_START_UPDATE = 0
BOARD_CONFIRM = 1
BOARD_VERSION = 2
BOARD_REBOOT = 3

args = parser.parse_args()

bus = can.interface.Bus(interface=interface, channel=args.channel, bitrate=250000)

filters = [
    {"can_id": PLATFORM_TX, "can_mask": 0x10f, "extended": False},
]
bus.set_filters(filters)

def can_recv(bus: can.BusABC, timeout=5):
    while True:
        rx_frame = bus.recv(timeout)
        if not rx_frame:
            raise BaseException("can receive data timeout")
        if rx_frame.arbitration_id == PLATFORM_TX:
            return struct.unpack('<2I', rx_frame.data)

def firmware_upgrade(file_name):
    with open(file_name, 'rb') as f:
        total_size = os.path.getsize(args.file)
        data = struct.pack('<2I', BOARD_START_UPDATE, total_size)
        msg = can.Message(arbitration_id=PLATFORM_RX, data=data, is_extended_id=False)
        bus.send(msg)
        f_offset = 0
        code, offset = can_recv(bus)
        if code != FW_CODE_OFFSET and offset != 0:
            raise BaseException(f"flash erase error: code({code}), offset({f_offset}, {offset})")
        bar = tqdm.tqdm(total=total_size)
        while True:
            chunk = f.read(8)
            if not chunk:
                break
            else:
                f_offset += len(chunk)
                bar.update(f_offset - bar.n)
            msg = can.Message(arbitration_id=FW_DATA_RX, data=chunk, is_extended_id=False)
            bus.send(msg)
            code, offset = can_recv(bus)
            if code == FW_CODE_UPDATE_SUCCESS and offset == f_offset:
                break
            if code != FW_CODE_OFFSET:
                raise BaseException(f"firmware upload error: code({code}), offset({f_offset}, {offset})")

        data = struct.pack('<2I', BOARD_CONFIRM, 0 if args.test else 1)
        msg = can.Message(arbitration_id=PLATFORM_RX, data=data, is_extended_id=False)
        bus.send(msg)
        code, offset = can_recv(bus, timeout=30)
        if code == FW_CODE_CONFIRM and offset == 0x55AA55AA:
            print(f"Image {args.file} upload finished, board will reboot and upgrade, it will take about 90~150s")
        elif code == FW_CODE_TRANFER_ERROR:
            print("Download Failed")


def firmware_version():
    data = struct.pack('<2I', BOARD_VERSION, 0)
    msg = can.Message(arbitration_id=PLATFORM_RX, data=data, is_extended_id=False)
    bus.send(msg)
    code, version = can_recv(bus)
    if code == FW_CODE_VERSION:
        ver1, ver2, ver3 =  version & 0xff, (version > 8) & 0xff, (version > 16) & 0xff
        print(f"version: v{ver1}.{ver2}.{ver3}")

def board_reboot():
    data = struct.pack('<2I', BOARD_REBOOT, 0)
    msg = can.Message(arbitration_id=PLATFORM_RX, data=data, is_extended_id=False)
    bus.send(msg)

if __name__ == "__main__":
    if args.command == 'flash':
        firmware_upgrade(args.file)
    elif args.command == 'board':
        if args.reboot:
            board_reboot()
        elif args.version:
            firmware_version()

