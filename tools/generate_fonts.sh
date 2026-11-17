#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
#
# Regenerates the LVGL font subsets in firmware/main from Montserrat SemiBold.
# The generated C files are committed, so building the firmware doesn't need
# Node. Run from the repository root:  sh tools/generate_fonts.sh
set -e

npm install --prefix .cache/font-tools lv_font_conv@1.5.3
CONV="node .cache/font-tools/node_modules/lv_font_conv/lv_font_conv.js"
TTF=assets/fonts/Montserrat-SemiBold.ttf
OUT=firmware/main

# Big numbers on the DRO and Spindle pages
$CONV --font $TTF --size 42 --bpp 4 --format lvgl --range 0x30-0x39 --symbols .- \
  --no-compress --lv-include lvgl.h --lv-font-name panel_digits_42 -o $OUT/font_digits42.c
$CONV --font $TTF --size 66 --bpp 4 --format lvgl --range 0x30-0x39 --symbols .- \
  --no-compress --lv-include lvgl.h --lv-font-name panel_digits_66 -o $OUT/font_digits66.c

# Classic gauge: RPM value, feed/positions and the "RPM" unit
$CONV --size 48 --bpp 4 --format lvgl --font $TTF --symbols 0123456789.- \
  --no-compress --no-prefilter --lv-include lvgl.h --lv-font-name panel_rpm_48 -o $OUT/font_rpm48.c
$CONV --size 21 --bpp 4 --format lvgl --font $TTF --symbols '0123456789.-F min/' \
  --no-compress --no-prefilter --lv-include lvgl.h --lv-font-name panel_text_21 -o $OUT/font_text21.c
$CONV --size 22 --bpp 4 --format lvgl --font $TTF --symbols RPM \
  --no-compress --no-prefilter --lv-include lvgl.h --lv-font-name panel_unit_22 -o $OUT/font_unit22.c
