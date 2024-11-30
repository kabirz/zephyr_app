#!/usr/bin/env python

# pip install smpclient tqdm

import argparse
import asyncio
from tqdm.asyncio import tqdm

from smpclient import SMPClient
from smpclient.generics import error, success
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite
from smpclient.requests.os_management import ResetWrite
from smpclient.transport.udp import SMPUDPTransport
from smpclient.transport.serial import SMPSerialTransport

class MySMPClient(SMPClient):
    async def connect(self, timeout_s: float = 5.0) -> None:
        await self._transport.connect(self._address, timeout_s)
        while True:
            try:
                await self.request(ImageStatesRead(), timeout_s=2)
                break
            except Exception:
                print('Wait connecting...')
                await asyncio.sleep(0.5)
        await self._initialize()

async def main() -> None:
    parser = argparse.ArgumentParser(description="Upload FW.")
    parser.add_argument("path", help="Path to the FW.")
    parser.add_argument("-t", "--test", action="store_true", help="test mode.")
    parser.add_argument("-a", "--address", help="ip address.")
    parser.add_argument("-d", "--device", help="serial port.")
    parser.add_argument("--mcuboot", action="store_true", help="mcuboot upload FW.")
    args = parser.parse_args()

    if args.address:
        transport = SMPUDPTransport()
        address = args.address
    elif args.device:
        transport = SMPSerialTransport()
        address = args.device
    else:
        print("must set ip adress or serial port")
        exit(-1)
    with open(args.path, "rb") as f:
        fw_file = f.read()

    async with MySMPClient(transport, address) as client:
        print(f"flashing {f.name}...")
        client._transport.initialize(512)
        proccess_bar = tqdm(total=len(fw_file))
        async for offset in client.upload(fw_file, upgrade=False):
            proccess_bar.update(offset - proccess_bar.n)
        proccess_bar.close()

        response = await client.request(ImageStatesRead())
        if success(response):
            if not args.mcuboot:
                response = await client.request(ImageStatesWrite(hash=response.images[1].hash, confirm=not args.test))
                response = await client.request(ResetWrite())
                if success(response):
                    print("flash firmware success, rebooting board...")
        elif error(response):
            print(f"Received error: {response}")
        else:
            raise Exception(f"Unknown response: {response}")

if __name__ == "__main__":
    asyncio.run(main())

