#!/usr/bin/env python

import os
import sys
import struct
import time

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} <fw_path>")
    exit(-1)

if not os.path.isfile(sys.argv[1]):
    print(f"File {sys.argv[1]} not found")
    exit(-1)


with open(sys.argv[1], 'rb') as input_f:
    with open(f'{sys.argv[1]}.csv', 'w') as output_f:

        out_str = 'timestamp,time,type'
        for i in range(16):
            out_str += f',channel{i+1}'
        out_str += '\n'
        output_f.write(out_str)
        while True:
            data = input_f.read(16)
            if len(data) < 16:
                break
            _type, timestamp, enable, *data = struct.unpack('=HI5H', data)
            t_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(timestamp))
            if _type == 1:
                out_str = f'{timestamp},{t_str},DI'
                for i in range(16):
                    if (1 << i) & enable:
                        out_str += ',ON' if ((1 << i) & data[0]) else ',OFF'
                    else:
                        out_str += ','
                out_str += '\n'
            elif _type == 2:
                out_str = f'{timestamp},{t_str},AI'
                val = ""
                for i in range(4):
                    if i < 2:
                        out_str += f',{data[i]/100.0}mA'
                    else:
                        out_str += f',{data[i]/100.0}V'
                out_str += '\n'
    
            output_f.write(out_str)
