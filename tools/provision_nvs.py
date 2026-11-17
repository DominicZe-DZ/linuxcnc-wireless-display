#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Builds a private NVS image with the display settings, to flash over USB.

Input JSON: ssid, password, optional machine_id and optional control_key.
Keep both the JSON and the .bin in local/ (git-ignored). The output is a full
NVS partition, not a patch: back up the flash before writing it.
"""
import argparse
import csv
import re
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("--output", type=Path, default=Path("local/panel-nvs.bin"))
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8"))
    if not 1 <= len(config["ssid"].encode("utf-8")) <= 31:
        parser.error("SSID must be 1 to 31 bytes of UTF-8")
    if len(config["password"].encode("utf-8")) > 63:
        parser.error("password can't be longer than 63 bytes")
    config['machine_id'] = config.get('machine_id', 'emco-f1')
    if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,31}', config['machine_id']):
        parser.error('invalid machine_id')
    if config.get('control_key') and not re.fullmatch(r'[0-9a-f]{64}', config['control_key']):
        parser.error('control_key must be 64 lower case hex characters')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(".csv")
    try:
        with temporary.open("w", newline="", encoding="utf-8") as file:
            writer = csv.writer(file)
            writer.writerow(["key", "type", "encoding", "value"])
            writer.writerow(["panel", "namespace", "", ""])
            for key in ("ssid", "password", "machine_id"):
                writer.writerow([key, "data", "string", config[key]])
            if config.get('control_key'):
                writer.writerow(['control_key', 'data', 'string', config['control_key']])
        subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen.nvs_partition_gen", "generate", str(temporary), str(args.output), "0x6000"], check=True)
    finally:
        temporary.unlink(missing_ok=True)
    print("NVS image written. It contains your Wi-Fi password, keep it out of Git")


if __name__ == "__main__":
    main()
