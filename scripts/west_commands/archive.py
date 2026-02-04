# Copyright (c) 2026 Kabirz
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import subprocess
import textwrap
import zipfile
from pathlib import Path
from typing import List, Tuple

from west.commands import WestCommand
from yaml import safe_load


def zip_files(files: List[Tuple[str, Path]], out_zip_file: Path) -> None:
    with zipfile.ZipFile(out_zip_file, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for outname, file in files:
            zipf.write(file, outname)

    print(f'Files compressed into {out_zip_file}')


class Archive(WestCommand):
    def __init__(self):
        super().__init__(
            "archive",
            "manage output for Zephyr",
            "Install all output into zip",
            accepts_unknown_args=True,
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
            epilog=textwrap.dedent(
                """
                archive all output image and other file
                to zip file.
            """
            ),
        )

        parser.add_argument('-d', '--build-dir', default='build',
                            help='build directory to create or use')
        parser.add_argument('--rebuild', action=argparse.BooleanOptionalAction,
                            help='manually specify to reinvoke cmake or not')
        parser.add_argument('--extra-file', nargs='*',
                            help='add list file to zip')

        return parser

    def do_run(self, args, _):
        build_dir = Path(args.build_dir)
        try:
            with open(build_dir/'build_info.yml') as f:
                info = safe_load(f)
        except FileNotFoundError:
            self.die("Can't found build_info.yml, Please rebuild this target")

        # rebuild target
        if args.rebuild is not False:
            west_info = info.get('west')
            if west_info:
                build_cmd = west_info.get('command')
                if build_cmd:
                    _cmd = build_cmd.split()
                    _cmd.remove('--cmake-only')
                    proc = subprocess.run(_cmd, encoding="utf-8")
                    if proc.returncode:
                        self.die('build error')
                else:
                    self.die('build error')
            else:
                self.die('build error')

        # compress file
        file_lists:List[Tuple[str, Path]] = []
        if args.extra_file:
            for _file in args.extra_file:
                _f = Path(_file)
                file_lists.append((_f.name, _file))
        app_name = "app"
        board_name = "common"
        cmake_info = info.get('cmake')
        board = cmake_info.get('board')
        if board:
            board_name = board['name']
        sysbuild = cmake_info.get('sysbuild')
        if sysbuild == 'true':
            hex_file_lists = []
            for image in cmake_info.get('images'):
                if image['type'] == 'BOOTLOADER':
                    bootloader_path = build_dir/image['name']
                    file_lists.append(('bootloader.bin', bootloader_path/'zephyr/zephyr.bin'))
                    file_lists.append(('bootloader.hex', bootloader_path/'zephyr/zephyr.hex'))
                    hex_file_lists.append(bootloader_path/'zephyr/zephyr.hex')
                elif image['type'] == 'MAIN':
                    app_path = build_dir/image['name']
                    file_lists.append(('app.bin', app_path/'zephyr/zephyr.signed.bin'))
                    file_lists.append(('app.hex', app_path/'zephyr/zephyr.signed.hex'))
                    hex_file_lists.append(app_path/'zephyr/zephyr.signed.hex')
                    app_name = image['name']
            if hex_file_lists:
                output_hex = build_dir/'full_output.hex'
                _cmd = ['hexmerge.py']
                _cmd.extend(hex_file_lists)
                _cmd.extend(['-o', output_hex.as_posix()])
                proc = subprocess.run(_cmd, encoding="utf-8")
                if proc.returncode:
                    self.die('hexmerge failed')
                file_lists.append(('full_image.hex', output_hex))
                # full bin image
                _cmd = ['hex2bin.py', output_hex, output_hex.with_suffix('.bin')]
                proc = subprocess.run(_cmd, encoding="utf-8")
                if proc.returncode:
                    self.die('hex2bin failed')
                file_lists.append(('full_image.bin', output_hex.with_suffix('.bin')))
        zip_files(file_lists, build_dir/f'{app_name}.{board_name}.zip')

