#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Starts panel_wifi straight from a checkout with LinuxCNC's own Python,
so no pip install is needed on the machine."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "host"))
from panel_wifi.app import main

if __name__ == "__main__":
    main()
