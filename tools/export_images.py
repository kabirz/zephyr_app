#!/usr/bin/env python

import os
import zipfile
from pathlib import Path
from typing import List, Tuple

from west.util import west_topdir

TOP_DIR = Path(west_topdir())
TOOL_DIR = TOP_DIR / 'apps/tools'
BUILD_DIR = TOP_DIR / 'build'


def generate_app():
    MCUBOOT_BIN = BUILD_DIR / 'mcuboot/zephyr/zephyr.bin'
    APP_SIGN_BIN = BUILD_DIR / 'data_collect/zephyr/zephyr.signed.bin'
    OUT_APP = BUILD_DIR / 'app.bin'

    with OUT_APP.open(mode='wb') as f:
        f.write(MCUBOOT_BIN.read_bytes())
        f.seek(128 * 1024, os.SEEK_SET)
        f.write(APP_SIGN_BIN.read_bytes())


def zip_files(files: List[Tuple[Path, str]], out_zip_file: Path):
    with zipfile.ZipFile(out_zip_file, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for file, path in files:
            zipf.write(file, os.path.join(path, os.path.basename(file)))

    print(f'Files compressed into {out_zip_file}')


files: List[Tuple[Path, str]] = [
    (BUILD_DIR / 'app.bin', 'images'),
    (BUILD_DIR / 'data_collect/zephyr/zephyr.signed.bin', 'images'),
    (TOOL_DIR / 'parser_raw.py', 'tools'),
    (TOOL_DIR / 'udp_multi_getinfo.py', 'tools'),
    (TOOL_DIR / 'udp_multi_setinfo.py', 'tools'),
    (TOOL_DIR / 'smp_upload.py', 'tools'),
]
generate_app()

zip_files(files, BUILD_DIR / 'daq_f407.zip')
