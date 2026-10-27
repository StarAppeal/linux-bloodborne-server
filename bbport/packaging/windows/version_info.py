#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes BLauncher.exe's version resource for PyInstaller (--version-file; package.sh runs it).

Windows Defender's machine learning flagged the v1.3.0 launcher as Trojan:Win32/Bearfoos.A!ml (player
report 2026-10-26): an unsigned PyInstaller exe with no version resource at all, so nothing in it says
what it is or who made it. The resource names the product, its version, its license and its home page.

Usage: version_info.py <release version, e.g. 1.3.1> <output file>
"""
import re
import sys
from pathlib import Path

PRODUCT = 'Yharnam Together: Bloodborne Seamless Co-op'
COMPANY = 'Yharnam Together'
DESCRIPTION = 'Yharnam Together launcher'
COPYRIGHT = 'GPL-2.0-or-later, https://github.com/lKaniel/bloodborne-server'


def numbers(version):
    """'1.3.1' or '1.3.1-rc.2' as the four numbers Windows keeps, (1, 3, 1, 0)."""
    found = [int(part) for part in re.findall(r'\d+', (version or '').split('-')[0])][:4]
    return tuple(found + [0] * (4 - len(found)))


def text(version):
    four = numbers(version)
    strings = {
        'CompanyName': COMPANY,
        'FileDescription': DESCRIPTION,
        'FileVersion': version or '0.0.0',
        'InternalName': 'BLauncher',
        'LegalCopyright': COPYRIGHT,
        'OriginalFilename': 'BLauncher.exe',
        'ProductName': PRODUCT,
        'ProductVersion': version or '0.0.0',
    }
    entries = ',\n'.join(f'          StringStruct({key!r}, {value!r})' for key, value in strings.items())
    return f'''VSVersionInfo(
  ffi=FixedFileInfo(filevers={four}, prodvers={four}, mask=0x3f, flags=0x0, OS=0x40004,
                    fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([
      StringTable('040904B0', [
{entries}
      ])
    ]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])
  ]
)
'''


def write(version, path):
    Path(path).write_text(text(version), encoding='utf-8')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[-1])
    write(sys.argv[1], sys.argv[2])
