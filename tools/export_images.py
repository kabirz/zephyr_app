#!/usr/bin/env python

import os
import zipfile
from pathlib import Path
from typing import List, Tuple

from west.util import west_topdir
import yaml
import argparse

parser = argparse.ArgumentParser('export images')
parser.add_argument('build_dir', type=str, help='spec build output name, not path')

args = parser.parse_args()

TOP_DIR = Path(west_topdir())
TOOL_DIR = TOP_DIR / 'apps/tools'
BUILD_DIR = TOP_DIR / args.build_dir
LOAD_SIZE = 0x20000

with open(BUILD_DIR/'build_info.yml', 'r') as f:
    info = yaml.safe_load(f)
    for vals in info['cmake']['images']:
        if vals['name'] == 'mcuboot':
            MCUBOOT_BIN = BUILD_DIR / 'mcuboot/zephyr/zephyr.bin'
            with open(BUILD_DIR/'mcuboot/zephyr/include/generated/zephyr/autoconf.h', 'r') as conf:
                for lines in conf.readlines():
                    if 'CONFIG_FLASH_LOAD_SIZE' in lines:
                        LOAD_SIZE = int((lines.split(' ')[-1][:-1]), 0)
        else:
            APP_NAME = vals["name"]
            APP_SIGN_BIN = BUILD_DIR / f'{APP_NAME}/zephyr/zephyr.signed.bin'
OUT_APP = BUILD_DIR / 'app.bin'
print(f'LOAD_SIZE is 0x{LOAD_SIZE:x}')


def generate_app() -> None:
    with OUT_APP.open(mode='wb') as f:
        f.write(MCUBOOT_BIN.read_bytes())
        f.seek(128 * 1024, os.SEEK_SET)
        f.write(APP_SIGN_BIN.read_bytes())


def zip_files(files: List[Tuple[Path, str]], out_zip_file: Path) -> None:
    with zipfile.ZipFile(out_zip_file, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for file, path in files:
            zipf.write(file, os.path.join(path, os.path.basename(file)))

    print(f'Files compressed into {out_zip_file}')


files: List[Tuple[Path, str]] = [
    (OUT_APP, 'images'),
    (APP_SIGN_BIN, 'images'),
    (TOOL_DIR / 'parser_raw.py', 'tools'),
    (TOOL_DIR / 'udp_multi_getinfo.py', 'tools'),
    (TOOL_DIR / 'udp_multi_setinfo.py', 'tools'),
    (TOOL_DIR / 'smp_upload.py', 'tools'),
]
generate_app()

zip_files(files, BUILD_DIR / f'{APP_NAME}.zip')

