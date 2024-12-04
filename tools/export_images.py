#!/usr/bin/env python

import os
import zipfile
from typing import List
from west.util import west_topdir
from pathlib import Path

TOP_DIR = Path(west_topdir())
TOOL_DIR = TOP_DIR/'apps/tools'

def generate_app():
    MCUBOOT_BIN = TOP_DIR/'build/mcuboot/zephyr/zephyr.bin'
    APP_SIGN_BIN = TOP_DIR/'build/data_collect/zephyr/zephyr.signed.bin'
    OUT_APP = TOP_DIR/'build/app.bin'

    with open(OUT_APP, 'wb') as f:
        f.write(open(MCUBOOT_BIN, 'rb').read())
        f.seek(128*1024, os.SEEK_SET)
        f.write(open(APP_SIGN_BIN, 'rb').read())


def zip_files(files: List[Path], out_zip_file: str):
    with zipfile.ZipFile(out_zip_file, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for file in files:
            zipf.write(file, os.path.basename(file))

    print(f'Files compressed into {out_zip_file}')


files: List[Path] = [
    TOP_DIR / 'build/app.bin',
    TOP_DIR / 'build/data_collect/zephyr/zephyr.signed.bin',
    TOOL_DIR / 'parser_raw.py',
    TOOL_DIR / 'udp_multi_getinfo.py',
    TOOL_DIR / 'udp_multi_setinfo.py',
    TOOL_DIR / 'smp_upload.py',
]
generate_app()

zip_files(files, 'daq_f407.zip')

