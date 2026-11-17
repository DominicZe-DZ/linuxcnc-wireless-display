# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Wire format, mirrored on the display side in firmware/main/telemetry.c.

JSON makes the link easy to debug with ordinary tools. Every datagram is a
complete snapshot, so nothing has to be rebuilt from deltas and old samples
are never retransmitted.
"""
import json
import math
import re

VERSION = 2
MAX_PACKET = 1200  # stays under a normal MTU, no IP fragmentation
NONCE = re.compile(r"^[0-9a-f]{16}$")
CHANNELS = {"rpm": "float", "feed": "float", "x": "float", "y": "float",
            "z": "float", "spindle_on": "bit", "coolant": "bit"}


def encode(message):
    """Refuses NaN/Infinity and oversize packets before anything is sent."""
    packet = json.dumps(message, separators=(",", ":"), allow_nan=False).encode("ascii")
    if len(packet) > MAX_PACKET:
        raise ValueError("message is larger than 1200 bytes")
    return packet


def hello_nonce(packet, machine, screen):
    """A hello only asks for data. It never runs anything or touches HAL."""
    if len(packet) > MAX_PACKET:
        return None
    try:
        value = json.loads(packet)
        if (isinstance(value, dict) and type(value.get("v")) is int
                and value["v"] == VERSION and value.get("type") == "hello"
                and value.get("machine") == machine and value.get("screen") == screen
                and isinstance(value.get("nonce"), str)
                and NONCE.fullmatch(value["nonce"])):
            return value["nonce"]
    except (ValueError, UnicodeError):
        pass
    return None


def sample_message(session, nonce, sequence, values, valid=True, demo=False,
                   machine="emco-f1", screen="001122334455"):
    """Builds a v2 sample. Each channel is checked against its declared type
    (CHANNELS); a wrong type or a non-finite number becomes None (JSON null),
    which the display shows as dashes."""
    clean = {}
    for name, kind in CHANNELS.items():
        value = values.get(name)
        if kind == "bit" and isinstance(value, bool):
            clean[name] = value
        elif kind == "float" and type(value) in (int, float) and math.isfinite(value):
            clean[name] = float(value)
        else:
            clean[name] = None  # missing is never disguised as a valid zero
    return {"v": VERSION, "type": "sample", "session": session, "nonce": nonce,
            "machine": machine, "screen": screen,
            "seq": sequence, "valid": bool(valid), "demo": bool(demo), "data": clean}
