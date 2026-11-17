#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Builds dashboard.c and pages.c against the board's LVGL and renders them
on the PC, no hardware involved. Images end up in .cache/native-preview."""
from pathlib import Path
import os
import subprocess

ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'.cache/native-preview'
out.mkdir(parents=True,exist_ok=True)
(out/'sdkconfig.h').write_text('#define CONFIG_PANEL_RPM_MAX 5000\n#define CONFIG_PANEL_RPM_WARNING 2500\n#define CONFIG_PANEL_BATTERY 1\n')
lvgl=Path(os.environ.get('PANEL_LVGL_DIR',ROOT/'firmware/components/lvgl__lvgl'))
sources=sorted((lvgl/'src').rglob('*.c'))
sources += [ROOT/'tests/render_dashboard.c']
sources += [ROOT/'firmware/main'/name for name in ('dashboard.c','pages.c','font_rpm48.c','font_text21.c','font_unit22.c','font_digits42.c','font_digits66.c')]
flags=['-DLV_CONF_SKIP=1','-DLV_COLOR_DEPTH=16','-DLV_MEM_SIZE=524288',
       '-DLV_FONT_MONTSERRAT_14=1','-DLV_FONT_MONTSERRAT_18=1','-DLV_FONT_MONTSERRAT_24=1','-DLV_FONT_MONTSERRAT_32=1']
binary=out/'render_dashboard'
subprocess.run(['gcc','-O2','-std=c99',*flags,'-I'+str(out),'-I'+str(lvgl),'-I'+str(ROOT/'firmware/main'),*map(str,sources),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary),str(out)],check=True)
