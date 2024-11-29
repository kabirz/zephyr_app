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

async def main() -> None:
    parser = argparse.ArgumentParser(description="Upload FW.")
    parser.add_argument("path", help="Path to the FW.")
    with open(parser.parse_args().path, "rb") as f:
        fw_file = f.read()

    async with SMPClient(SMPUDPTransport(), "192.168.12.101") as client:
        print(f"flashing {f.name}...")
        client._transport.initialize(512)
        proccess_bar = tqdm(total=len(fw_file))
        async for offset in client.upload(fw_file, upgrade=False):
            proccess_bar.update(offset - proccess_bar.n)
        proccess_bar.close()

        response = await client.request(ImageStatesRead())
        if success(response):
            response = await client.request(ImageStatesWrite(hash=response.images[1].hash, confirm=True))
            if success(response):
                response = await client.request(ResetWrite())
                if success(response):
                    print("flash firmware success, rebooting board...")
        elif error(response):
            print(f"Received error: {response}")
        else:
            raise Exception(f"Unknown response: {response}")

if __name__ == "__main__":
    asyncio.run(main())

