#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Puts a firmware image where panel_wifi offers it to the display (OTA).

Copies the application binary to <data_dir>/firmware/display.bin and writes
display.json next to it with the build ID and SHA-256. Run it on the LinuxCNC
PC, then restart LinuxCNC (or just the panel_wifi component) so the sender
picks the new image up. The display updates itself the next time it is in
idle manual mode.

    python3 tools/publish_firmware.py firmware/build/linuxcnc_wireless_display.bin

The build ID is read from firmware/main/version.h unless --version is given.
It must match the PANEL_BUILD_ID compiled into that binary.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]


def build_id_from_header():
    text = (ROOT / 'firmware/main/version.h').read_text(encoding='utf-8')
    match = re.search(r'PANEL_BUILD_ID\s+"([^"]+)"', text)
    return match.group(1) if match else None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=Path, help='application .bin built by idf.py')
    parser.add_argument('--version', help='build ID (default: firmware/main/version.h)')
    parser.add_argument('--data-dir', type=Path, default=ROOT / 'local',
                        help='same folder as data_dir in host.json (default: <repo>/local)')
    args = parser.parse_args()

    version = args.version or build_id_from_header()
    if not version or not re.fullmatch(r'[0-9a-z]{1,15}', version) or version == 'nobuildid':
        parser.error('no usable build ID; pass --version with the one inside the image')
    data = args.image.read_bytes()
    if not 1024 <= len(data) <= 3 * 1024 * 1024:
        parser.error('image must be between 1 KiB and 3 MiB')
    if version.encode() not in data:
        parser.error(f'build ID {version!r} not found inside {args.image}')

    folder = args.data_dir.expanduser() / 'firmware'
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.image, folder / 'display.bin')
    record = {'version': version, 'bytes': len(data),
              'sha256': hashlib.sha256(data).hexdigest()}
    (folder / 'display.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(f'published {version} ({len(data)} bytes) in {folder}')


if __name__ == '__main__':
    main()
