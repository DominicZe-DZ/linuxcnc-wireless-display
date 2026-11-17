#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Soft-limit check for the program written by generate_machine_demo.py.

Run it on the LinuxCNC PC. It only reads the INI and linuxcnc.stat, it never
creates a linuxcnc.command. It checks travel against the soft limits with the
current G54; it knows nothing about fixtures, tools or the part. It is not
meant for arbitrary G-code.
"""
import argparse
import configparser
import json
from pathlib import Path
import re


def check_points(program, offsets, current_machine, limits):
    point=[current_machine[i]-offsets[i] for i in range(3)]
    envelope=[[current_machine[i],current_machine[i]] for i in range(3)]
    errors=[]
    count=0
    if 'G21 G17 G90 G94 G54 G40 G80' not in program:
        raise ValueError('this file was not written by generate_machine_demo.py')
    for line_number,line in enumerate(program.splitlines(),1):
        if not line.startswith('G1 '):continue
        for axis,value in re.findall(r'([XYZ])(-?\d+(?:\.\d+)?)',line):
            point['XYZ'.index(axis)]=float(value)
        count+=1
        for i,axis in enumerate('XYZ'):
            machine=point[i]+offsets[i]
            envelope[i][0]=min(envelope[i][0],machine)
            envelope[i][1]=max(envelope[i][1],machine)
            minimum,maximum=limits[i]
            if not minimum<=machine<=maximum:
                errors.append({'line':line_number,'axis':axis,'machine_mm':round(machine,4),'limits':limits[i]})
    if not count:raise ValueError('no G1 moves found')
    return {'valid_limits':not errors,'moves':count,'machine_envelope':dict(zip('XYZ',envelope)),
            'errors':errors,'executed':False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ini',type=Path,required=True)
    parser.add_argument('program',type=Path)
    args=parser.parse_args()
    import linuxcnc
    status=linuxcnc.stat()
    status.poll()
    if status.g5x_index!=1:
        parser.error('select G54 in the GUI first')
    if not all(status.homed[:3]):parser.error('home all three axes first')
    if abs(status.rotation_xy)>1e-9 or any(abs(v)>1e-9 for v in (*status.g92_offset[:3],*status.tool_offset[:3])):
        parser.error('needs G54 with no rotation, no G92 and no tool offset')
    config=configparser.ConfigParser(interpolation=None,strict=False)
    config.read(args.ini)
    limits=[(config.getfloat('AXIS_'+axis,'MIN_LIMIT'),config.getfloat('AXIS_'+axis,'MAX_LIMIT')) for axis in 'XYZ']
    report=check_points(args.program.read_text(encoding='ascii'),status.g5x_offset[:3],status.actual_position[:3],limits)
    print(json.dumps(report,indent=2))
    raise SystemExit(0 if report['valid_limits'] else 2)


if __name__=='__main__':main()
