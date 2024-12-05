#!/usr/bin/env python

from west.util import west_topdir
from pathlib import Path
import subprocess

TOP_DIR = Path(west_topdir())
patches_path = TOP_DIR / 'apps/patches'


# mcuboot
cmd = ['west', 'update', 'mcuboot']
subprocess.call(cmd)

cmd = [
	'git',
	'-C',
	TOP_DIR / 'bootloader/mcuboot',
	'am',
	patches_path / '0001-mcuboot-support-udp-upgrade.patch',
]
subprocess.call(cmd)
