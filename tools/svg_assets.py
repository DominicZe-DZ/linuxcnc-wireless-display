# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""SVG and logo helpers shared by the asset generators."""
import base64
from io import BytesIO
from pathlib import Path
import xml.etree.ElementTree as ET
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
NS = '{http://www.w3.org/2000/svg}'
ET.register_namespace('', NS[1:-1])


def el(tag, **attributes):
    """SVG element; keyword underscores become dashes (stroke_width=...)."""
    return ET.Element(NS + tag, {k.replace('_', '-'): str(v) for k, v in attributes.items()})


def rgba_logo(stacked=False):
    """DominicZe logo cropped from the original, white with alpha, as base64 PNG.

    The source is white on black, so luminance becomes the alpha channel.
    stacked=True puts the gear above the name (Classic gauge), otherwise the
    gear sits on the left.
    """
    alpha = ImageOps.grayscale(Image.open(ROOT / 'assets/dz-original.png').convert('RGB'))
    if stacked:
        size = (164, 118)
        parts = [((541, 220, 971, 612), (46, 0, 72, 66)),
                 ((448, 628, 1022, 736), (0, 74, 164, 31)),
                 ((448, 752, 1022, 792), (5, 107, 154, 11))]
    else:
        size = (210, 64)
        parts = [((541, 220, 971, 612), (0, 0, 70, 64)),
                 ((448, 628, 1022, 736), (79, 12, 131, 25)),
                 ((448, 752, 1022, 792), (79, 43, 131, 9))]
    coverage = Image.new('L', size, 0)
    for crop, (x, y, w, h) in parts:
        coverage.paste(alpha.crop(crop).resize((w, h), Image.Resampling.LANCZOS), (x, y))
    image = Image.new('RGBA', size, 'white')
    image.putalpha(coverage)
    buffer = BytesIO()
    image.save(buffer, format='PNG')
    return base64.b64encode(buffer.getvalue()).decode('ascii')


def wordmark(filename, x, y, width, height, color):
    """Nested SVG that keeps the source viewBox, so no letter gets clipped."""
    source = ET.parse(ROOT / 'assets' / filename).getroot()
    group = el('svg', x=x, y=y, width=width, height=height,
               viewBox=source.attrib['viewBox'], color=color, fill=color)
    for path in source.iter(NS + 'path'):
        attrs = dict(path.attrib)
        attrs['fill'] = color
        attrs.pop('id', None)
        group.append(ET.Element(NS + 'path', attrs))
    group.set('fill-rule', 'evenodd')
    return group


def status_icon(kind):
    """32x32 spindle / coolant indicator, drawn with currentColor strokes."""
    group = el('g', fill='none', stroke='currentColor', stroke_width='2.2',
               stroke_linecap='round', stroke_linejoin='round')
    if kind == 'spindle':
        # Head, tool holder, cutter and a rotation arrow.
        group.append(el('rect', x=9, y=3, width=14, height=6, rx=1))
        group.append(el('path', d='M10 9V18L13 21H19L22 18V9 M14 21V29H18V21 M14 27L18 24'))
        group.append(el('path', d='M27 12C31 17 29 21 25 24 M25 20V24H29'))
    else:
        # A big drop over a liquid wave.
        group.append(el('path', d='M16 2C12 8 8 11 8 16A8 8 0 0 0 24 16C24 11 20 8 16 2Z',
                        fill='currentColor', fill_opacity='.18'))
        group.append(el('path', d='M12 16C12 19 14 21 17 21 M3 28Q6 24 10 28T18 28T26 28L29 26'))
    return group
