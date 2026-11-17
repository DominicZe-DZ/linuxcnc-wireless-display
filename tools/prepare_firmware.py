#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Pulls the board drivers and LVGL out of the official Waveshare demo.

The demo isn't redistributed here because its license isn't stated. This
script downloads it from Waveshare, checks the SHA-256 and copies only what
the firmware needs into git-ignored folders. Use --archive to point at a zip
you already have.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
URL = "https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-Demo.zip"
SHA256 = "f93eff03664f7dded6f91f9c362fda0d7c870991b4fa99ccbcfdd3e57ce39cf5"
PREFIX = "ESP-IDF/ESP32-S3-Touch-LCD-2.8C-Test/"
DRIVERS = ("EXIO", "Buzzer", "I2C_Driver", "LCD_Driver", "Touch_Driver", "LVGL_Driver")


def prepare_headers():
    """Adds the FreeRTOS includes the vendor headers rely on implicitly.

    The demo's own LVGL port isn't built (display_port.c replaces it), but
    the LCD and touch drivers keep their headers. FreeRTOS.h has to come
    before semphr.h.
    """
    folder = ROOT / "firmware/main/vendor"
    for relative in ("LCD_Driver/ST7701S.h", "LVGL_Driver/LVGL_Driver.h"):
        header = folder / relative
        includes = '#include "freertos/FreeRTOS.h"\n#include "freertos/semphr.h"\n'
        header.write_text(includes + header.read_text(encoding="utf-8"), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    archive = args.archive or ROOT / ".cache" / "waveshare-demo.zip"
    if not archive.exists():
        if args.archive:
            parser.error("archive not found")
        archive.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(URL, timeout=120) as response, archive.open("wb") as out:
            shutil.copyfileobj(response, out)
    with archive.open("rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != SHA256:
        parser.error("the demo zip changed (SHA-256 mismatch), nothing imported")
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            if item.is_dir() or not item.filename.startswith(PREFIX):
                continue
            relative = item.filename[len(PREFIX):]
            if relative.startswith("components/lvgl__lvgl/"):
                target = ROOT / "firmware" / relative
            elif any(relative.startswith(f"main/{driver}/") for driver in DRIVERS):
                target = ROOT / "firmware/main/vendor" / relative[len("main/"):]
            else:
                continue
            if not target.resolve().is_relative_to((ROOT / "firmware").resolve()):
                parser.error("unexpected path inside the vendor archive")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.read(item))
    prepare_headers()
    print("drivers ready, SHA-256 verified")


if __name__ == "__main__":
    main()
