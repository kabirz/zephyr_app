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
DOCS_DIR = TOP_DIR / 'apps/docs'
BUILD_DIR = TOP_DIR / args.build_dir
LOAD_SIZE = 0x20000
BOARD_NAME = 'daq_f407vet6'

with open(BUILD_DIR/'build_info.yml', 'r') as f:
    info = yaml.safe_load(f)
    BOARD_NAME = info['cmake']['board']['name']
    QUALIFIERS = info['cmake']['board']['qualifiers']
    for vals in info['cmake']['images']:
        if vals['name'] == 'mcuboot':
            MCUBOOT_BIN = BUILD_DIR / 'mcuboot/zephyr/zephyr.bin'
            MCUBOOT_HEX_BIN = BUILD_DIR / 'mcuboot/zephyr/zephyr.hex'
            with open(BUILD_DIR/'mcuboot/zephyr/include/generated/zephyr/autoconf.h', 'r') as conf:
                for lines in conf.readlines():
                    if 'CONFIG_FLASH_LOAD_SIZE' in lines:
                        LOAD_SIZE = int((lines.split(' ')[-1][:-1]), 0)
        else:
            APP_NAME = vals["name"]
            APP_SIGN_BIN = BUILD_DIR / f'{APP_NAME}/zephyr/zephyr.signed.bin'
            APP_HEX_BIN = BUILD_DIR / f'{APP_NAME}/zephyr/zephyr.signed.hex'
OUT_APP = BUILD_DIR / 'app_full.bin'
print(f'board:{BOARD_NAME}, app: {APP_NAME}, LOAD_SIZE is 0x{LOAD_SIZE:x}')


def generate_app() -> None:
    with OUT_APP.open(mode='wb') as f:
        f.write(MCUBOOT_BIN.read_bytes())
        f.seek(LOAD_SIZE, os.SEEK_SET)
        f.write(APP_SIGN_BIN.read_bytes())


def zip_files(files: List[Tuple[Path, str, str]], out_zip_file: Path) -> None:
    with zipfile.ZipFile(out_zip_file, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for file, path , name in files:
            if name:
                zipf.write(file, os.path.join(path, name))
            else:
                zipf.write(file, os.path.join(path, os.path.basename(file)))

    print(f'Files compressed into {out_zip_file}')

generate_app()

if (BOARD_NAME == 'daq_f407vet6' or 'daq' in QUALIFIERS) and APP_NAME == 'data_collect':
    files0: List[Tuple[Path, str, str]] = [
        (OUT_APP, 'images', ''),
        (APP_SIGN_BIN, 'images', 'app_signed.bin'),
        (TOOL_DIR / 'parser_raw.py', 'tools', ''),
        (TOOL_DIR / 'udp_multi_getinfo.py', 'tools', ''),
        (TOOL_DIR / 'udp_multi_setinfo.py', 'tools', ''),
        (TOOL_DIR / 'smp_upload.py', 'tools', ''),
    ]
    zip_files(files0, BUILD_DIR / f'{APP_NAME}.zip')
elif (BOARD_NAME in ('monv_f407vet6', 'laser_f103ret7') or 'monv' in QUALIFIERS)\
    and APP_NAME == 'laser_ctrl':
    files1: List[Tuple[Path, str, str]] = [
        (OUT_APP, 'images', ''),
        (MCUBOOT_HEX_BIN, 'images', 'mcuboot.hex'),
        (APP_SIGN_BIN, 'images', 'app_signed.bin'),
        (APP_HEX_BIN, 'images', 'app_signed.hex'),
        (TOOL_DIR / 'can_upgrade.py', 'tools', 'can_download.py'),
        (DOCS_DIR/ '刷机.docx', 'docs', ''),
        (DOCS_DIR/ '单点激光板测试.docx', 'docs', ''),
        (DOCS_DIR/ '固件升级.docx', 'docs', ''),
    ]

    zip_files(files1, BUILD_DIR / f'{BOARD_NAME}_{APP_NAME}.zip')

