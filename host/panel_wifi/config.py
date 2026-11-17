# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Host settings, validated before any HAL pin or socket is created."""
from dataclasses import dataclass
import ipaddress
import json
import math
from pathlib import Path
import re

# Default data folder: local/ at the root of the repository checkout. It is
# git-ignored and also holds host.json, so everything private stays together.
DEFAULT_DATA_DIR = Path(__file__).resolve().parents[2] / 'local'


@dataclass(frozen=True)
class Config:
    """Contents of host.json. Every key except interface and screen_id has a
    default. See docs/LINUXCNC_SETUP.md for what each one means."""
    interface: str
    screen_id: str
    machine_id: str = "emco-f1"
    host_port: int = 27180
    screen_port: int = 27181
    rate_hz: int = 25
    mesa_network: str = "10.10.10.0/24"
    rpm_input_scale: float = 1.0
    control_key: str = ''  # empty = read-only, no control channel
    firmware_port: int = 27190  # 0 disables the OTA server
    data_dir: str = ''  # log and firmware folder, '' = <repo>/local

    def data_path(self):
        return Path(self.data_dir).expanduser() if self.data_dir else DEFAULT_DATA_DIR

    @classmethod
    def load(cls, path):
        with open(path, encoding="utf-8") as stream:
            result = cls(**json.load(stream))
        ipaddress.IPv4Network(result.mesa_network)
        if not isinstance(result.control_key, str) or (result.control_key
                and not re.fullmatch(r'[0-9a-f]{64}', result.control_key)):
            raise ValueError('control_key must be empty or 64 lower case hex characters')
        if not isinstance(result.screen_id, str) or not re.fullmatch(r"[0-9a-f]{12}", result.screen_id):
            raise ValueError("screen_id must be the display Wi-Fi MAC, lower case, no separators")
        if not isinstance(result.machine_id, str) or not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,31}", result.machine_id):
            raise ValueError("machine_id must be 1-32 lower case letters, digits, '-' or '_'")
        if not result.interface or len(result.interface) > 15 or "/" in result.interface:
            raise ValueError("invalid interface name")
        for port in (result.host_port, result.screen_port):
            if type(port) is not int or not 1024 <= port <= 65535:
                raise ValueError("ports must be between 1024 and 65535")
        if type(result.rate_hz) is not int or not 1 <= result.rate_hz <= 100:
            raise ValueError("rate_hz must be between 1 and 100")
        if (type(result.rpm_input_scale) not in (int, float)
                or not math.isfinite(result.rpm_input_scale)
                or not 0 < result.rpm_input_scale <= 1000):
            raise ValueError("rpm_input_scale must be a positive, finite multiplier")
        if (type(result.firmware_port) is not int
                or not 0 <= result.firmware_port <= 65535):
            raise ValueError('firmware_port must be 0 (disabled) or a valid port')
        if not isinstance(result.data_dir, str):
            raise ValueError('data_dir must be a path')
        return result
