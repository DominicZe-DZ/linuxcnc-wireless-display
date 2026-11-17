# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Bounding box and states of the generated demo program, nothing is run."""
import importlib.util
from pathlib import Path
import re
import unittest

spec=importlib.util.spec_from_file_location('machine_demo',Path(__file__).resolve().parents[1]/'tools/generate_machine_demo.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
checker_spec=importlib.util.spec_from_file_location('demo_checker',Path(__file__).resolve().parents[1]/'tools/check_machine_demo.py')
checker=importlib.util.module_from_spec(checker_spec)
checker_spec.loader.exec_module(checker)


class DemoProgramTests(unittest.TestCase):
    def test_closed_xyz_path_inside_declared_box(self):
        program,report=module.make_program(35,30,35,500,4)
        points=[]
        for line in program.splitlines():
            match=re.fullmatch(r'G1 X(-?[0-9.]+) Y(-?[0-9.]+) Z(-?[0-9.]+)',line)
            if match:points.append(tuple(map(float,match.groups())))
        self.assertEqual(len(points),289)  # approach + 4x72 XYZ points
        for axis,half in enumerate((35,30,35)):
            self.assertEqual(min(p[axis] for p in points),-half)
            self.assertEqual(max(p[axis] for p in points),half)
        for cycle in range(4):self.assertEqual(points[(cycle+1)*72],(35,0,0))
        self.assertEqual(report['coordinated_segments'],288)
        self.assertFalse(report['executed'])

    def test_spindle_coolant_and_program_end(self):
        program,_=module.make_program(1,1,1,500,4)
        lines=program.splitlines()
        self.assertEqual(lines.count('M3 S1000'),4)
        self.assertEqual(lines.count('S5000'),4)
        self.assertEqual(lines.count('G4 P5'),4)
        self.assertEqual(lines.count('M7'),4)
        self.assertEqual(lines.count('M8'),8)
        self.assertEqual(lines[-4:],['M5','S0','M9','M2'])
        self.assertNotIn('G10',program)
        self.assertNotIn('G53',program)

    def test_rejects_bad_demo_parameters(self):
        for args in ((0,1,1,500,4),(1,1,float('nan'),500,4),(1,1,1,500,0),(1,1,1,2000,4)):
            with self.assertRaises(ValueError):module.make_program(*args)

    def test_preflight_catches_work_offset_outside_axis_limit(self):
        limits=[(0,195),(0,100),(-198,0)]
        offsets=[100,34,-100]
        requested,_=module.make_program(35,35,35)
        invalid=checker.check_points(requested,offsets,offsets,limits)
        self.assertFalse(invalid['valid_limits'])
        self.assertTrue(all(e['axis']=='Y' for e in invalid['errors']))
        adjusted,_=module.make_program(35,30,35)
        self.assertTrue(checker.check_points(adjusted,offsets,offsets,limits)['valid_limits'])


if __name__=='__main__':unittest.main()
