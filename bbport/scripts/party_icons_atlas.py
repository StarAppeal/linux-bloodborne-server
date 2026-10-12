#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the party death icons (gpu/art/party_icons.svg) into the atlas the overlay embeds (gpu/art/party_icons.bin):
'BBIC', width, height and icon count (little-endian u16 each), then the RGBA pixels, zlib-compressed. A developer
tool, run after editing the drawing: it needs a Chromium browser (Edge or Chrome) to draw the SVG and Pillow."""
import argparse
import io
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

PORT = Path(__file__).resolve().parent.parent
SVG = PORT / 'gpu/art/party_icons.svg'
ATLAS = PORT / 'gpu/art/party_icons.bin'
ICON, COUNT = 128, 7
BROWSERS = (r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
            r'C:\Program Files\Google\Chrome\Application\chrome.exe', 'chromium', 'google-chrome')


def encode(width, height, rgba):
    if len(rgba) != width * height * 4 or width % ICON or width // ICON != COUNT:
        raise ValueError('the atlas is a row of 128-pixel RGBA icons')
    return b'BBIC' + struct.pack('<HHH', width, height, COUNT) + zlib.compress(bytes(rgba), 9)


def decode(data):
    """(width, height, count, rgba) of an atlas file (the overlay reads it the same way)."""
    if data[:4] != b'BBIC':
        raise ValueError('not a party icon atlas')
    width, height, count = struct.unpack_from('<HHH', data, 4)
    rgba = zlib.decompress(data[10:])
    if len(rgba) != width * height * 4:
        raise ValueError('the atlas has the wrong number of pixels')
    return width, height, count, rgba


def draw(browser, svg, png):
    with tempfile.TemporaryDirectory() as folder:
        page = Path(folder) / 'atlas.html'
        page.write_text('<html><body style="margin:0;background:transparent">'
                        f'{svg.read_text(encoding="utf-8")}</body></html>', encoding='utf-8')
        subprocess.run([browser, '--headless=new', '--disable-gpu', '--hide-scrollbars',
                        '--default-background-color=00000000', '--force-device-scale-factor=1',
                        f'--window-size={ICON * COUNT},{ICON}', f'--screenshot={png}', page.as_uri()],
                       check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', help='a Chromium browser (default: Edge, then Chrome)')
    args = parser.parse_args()
    browser = args.browser or next((b for b in BROWSERS if Path(b).is_file() or shutil.which(b)), None)
    if not browser:
        raise SystemExit('No Chromium browser found: pass --browser')
    from PIL.PngImagePlugin import PngImageFile
    with tempfile.TemporaryDirectory() as folder:
        png = Path(folder) / 'atlas.png'
        draw(browser, SVG, png)
        image = PngImageFile(io.BytesIO(png.read_bytes())).convert('RGBA')
        if image.size != (ICON * COUNT, ICON):
            raise SystemExit(f'the browser drew {image.size}, not {(ICON * COUNT, ICON)}')
        ATLAS.write_bytes(encode(*image.size, image.tobytes()))
    print(f'{ATLAS}: {ATLAS.stat().st_size} bytes')


if __name__ == '__main__':
    main()
