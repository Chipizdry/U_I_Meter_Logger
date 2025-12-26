


#!/usr/bin/env python3
"""
Простой скрипт для создания merged.bin для ESP32.
Требует: esptool.py в PATH и Python 3.


Использование:
python3 merge_bins.py --boot build/bootloader/bootloader.bin \
--part build/partition_table/partition-table.bin \
--app build/app.bin \
--fs build/littlefs.bin \
--out merged.bin \
--flash_mode dio --flash_freq 40m --flash_size 4MB


Скрипт делает простую команду esptool merge_bin с указанными оффсетами.
"""


import argparse
import subprocess
import sys


DEFAULTS = {
'boot_offset': '0x1000',
'part_offset': '0x8000',
'app_offset': '0x10000',
'fs_offset': '0x430000',
}


parser = argparse.ArgumentParser()
parser.add_argument('--boot', required=True, help='bootloader bin')
parser.add_argument('--part', required=True, help='partition table bin')
parser.add_argument('--app', required=True, help='app bin')
parser.add_argument('--fs', required=False, help='littlefs bin')
parser.add_argument('--out', required=True, help='output merged.bin')
parser.add_argument('--flash_mode', default='dio')
parser.add_argument('--flash_freq', default='40m')
parser.add_argument('--flash_size', default='4MB')
parser.add_argument('--boot_offset', default=DEFAULTS['boot_offset'])
parser.add_argument('--part_offset', default=DEFAULTS['part_offset'])
parser.add_argument('--app_offset', default=DEFAULTS['app_offset'])
parser.add_argument('--fs_offset', default=DEFAULTS['fs_offset'])
args = parser.parse_args()


cmd = [
'esptool.py', '--chip', 'esp32', 'merge_bin',
'-o', args.out,
'--flash_mode', args.flash_mode,
'--flash_freq', args.flash_freq,
'--flash_size', args.flash_size,
args.boot_offset, args.boot,
args.part_offset, args.part,
args.app_offset, args.app,
]


if args.fs:
 cmd += [args.fs_offset, args.fs]


print('Running: ' + ' '.join(cmd))
rc = subprocess.call(cmd)
if rc != 0:
 print('esptool failed', file=sys.stderr)
 print('merged.bin created: ' + args.out)


 