# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""No machine needed: permissions, fixed command set, signatures, expiry, offsets."""
from dataclasses import replace
import json
import math
from types import SimpleNamespace
import time
import unittest
from panel_wifi.config import Config
from panel_wifi.cnc_status import permission, work_position
from panel_wifi.control import Control, MdiExecutor, canonical, signature


class FakeCnc:
    MODE_MANUAL, MODE_AUTO, MODE_MDI = 1, 2, 3
    STATE_ON, INTERP_IDLE, INTERP_PAUSED, EXEC_DONE, RCS_DONE = 4, 1, 3, 1, 1

    def __init__(self):
        self.s = SimpleNamespace(task_mode=1, task_state=4, task_paused=False, paused=False,
            interp_state=1, exec_state=1, queue=0, active_queue=0, inpos=True, current_vel=0.,
            estop=False, enabled=True, homed=[1,1,1], joints=3, gcodes=[], g5x_index=2,
            spindle=[{'enabled':False,'override':1.}], settings=[0,0,1500],
            actual_position=[125.,-75.,-120.], g5x_offset=[100.,-50.,-100.],
            g92_offset=[0.,0.,0.], tool_offset=[0.,0.,0.], rotation_xy=0., linear_units=1.)
        self.s.poll = lambda: None
        self.blocks=[]
        self.race=False

    def stat(self): return self.s
    def command(self): return self
    def wait_complete(self, timeout): return self.RCS_DONE
    def mode(self, mode):
        if self.race and mode == self.MODE_MDI:
            self.s.task_mode=self.MODE_AUTO
            self.s.interp_state=2
        else: self.s.task_mode=mode
    def mdi(self, block):
        self.blocks.append(block)
        if 'M3' in block: self.s.spindle[0]['enabled']=True
        if 'M5' in block: self.s.spindle[0]['enabled']=False
        for word in block.split():
            if word.startswith('S'): self.s.settings[2]=int(word[1:])
            if word in ['X0','Y0','Z0']:
                i='XYZ'.index(word[0]);self.s.g5x_offset[i]=self.s.actual_position[i]


class CncTests(unittest.TestCase):
    def test_permissions_cover_program_pause_pending_mdi_off_motion_and_override(self):
        cnc=FakeCnc()
        self.assertEqual(permission(cnc.s,cnc),'manual')
        for field,value in [('task_mode',2),('task_paused',True),('paused',True),
                            ('task_mode',3),('enabled',False),('queue',1),('active_queue',1),
                            ('exec_state',2),('inpos',False),('current_vel',1.)]:
            old=getattr(cnc.s,field);setattr(cnc.s,field,value)
            self.assertNotEqual(permission(cnc.s,cnc),'manual',field)
            setattr(cnc.s,field,old)
        cnc.s.gcodes=[960];self.assertEqual(permission(cnc.s,cnc),'css')
        cnc.s.gcodes=[];cnc.s.spindle[0]['override']=1.2
        self.assertEqual(permission(cnc.s,cnc),'override')

    def test_work_offsets_rotation_and_machine_units(self):
        cnc=FakeCnc()
        self.assertEqual(work_position(cnc.s),[25.,-25.,-20.])
        cnc.s.rotation_xy=90
        cnc.s.g92_offset=[1,2,3]
        result=work_position(cnc.s)
        self.assertAlmostEqual(result[0],-26)
        self.assertAlmostEqual(result[1],-27)
        self.assertAlmostEqual(result[2],-23)
        cnc.s.linear_units=1/25.4
        self.assertAlmostEqual(work_position(cnc.s)[0],-26*25.4)

    def test_mdi_commands_do_not_move_axes_and_speed_when_stopped_is_guarded(self):
        cnc=FakeCnc();executor=MdiExecutor(cnc)
        request={'action':'speed','rpm':2500,'axis':'-','wcs':2}
        self.assertEqual(executor.execute(request),'stopped')
        self.assertEqual(cnc.blocks,[])
        self.assertEqual(cnc.s.settings[2],1500)
        cnc.s.spindle[0]['enabled']=True
        self.assertEqual(executor.execute(request),'ok')
        self.assertEqual(cnc.blocks,['S2500'])
        self.assertTrue(cnc.s.spindle[0]['enabled'])
        self.assertEqual(executor.execute(dict(request,action='stop')),'ok')
        self.assertFalse(cnc.s.spindle[0]['enabled'])
        position=list(cnc.s.actual_position)
        self.assertEqual(executor.execute(dict(request,action='zero',axis='x')),'ok')
        self.assertEqual(work_position(cnc.s)[0],0)
        self.assertEqual(cnc.s.actual_position,position)
        self.assertIn('G10 L20 P2 X0',cnc.blocks)

    def test_speed_guard_uses_real_hal_rotation_besides_modal_m3(self):
        # Bare S word: the drive turns (HAL) with enabled FALSE. Speed is legit.
        cnc=FakeCnc();executor=MdiExecutor(cnc)
        request={'action':'speed','rpm':2500,'axis':'-','wcs':2,'_spindle_running':True}
        self.assertEqual(executor.execute(request),'ok')
        self.assertEqual(cnc.blocks,['S2500'])
        self.assertFalse(cnc.s.spindle[0]['enabled'])
        # Both say stopped: keep the guard (an S word would start the drive).
        quiet=FakeCnc();executor=MdiExecutor(quiet)
        self.assertEqual(executor.execute(dict(request,_spindle_running=False)),'stopped')
        self.assertEqual(quiet.blocks,[])

    def test_speed_revalidates_real_rotation_before_the_block(self):
        # Someone presses Stop between admission and the block: no bare S
        # word that would start the spindle again.
        cnc=FakeCnc();executor=MdiExecutor(cnc)
        request={'action':'speed','rpm':2500,'axis':'-','wcs':2,'_spindle_running':True}
        self.assertEqual(executor.execute(request, live_running=lambda: False), 'stopped')
        self.assertEqual(cnc.blocks,[])
        self.assertEqual(executor.execute(request, live_running=lambda: True), 'ok')
        self.assertEqual(cnc.blocks,['S2500'])

    def test_expired_budget_never_enters_mdi(self):
        cnc=FakeCnc();executor=MdiExecutor(cnc)
        request={'action':'start','rpm':1000,'axis':'-','wcs':2,'_valid_for':0.0}
        self.assertEqual(executor.execute(request), 'expired')
        self.assertEqual(cnc.blocks,[])
        self.assertEqual(cnc.s.task_mode, cnc.MODE_MANUAL)

    def test_stop_is_accepted_in_safe_degraded_states_but_not_with_program(self):
        # Stop must work in degraded states too: override, not homed, CSS or
        # a busy task (the host checks again).
        for reason, setup in (
                ('override', lambda c: c.s.spindle[0].__setitem__('override', 1.2)),
                ('home', lambda c: c.s.__setattr__('homed', [0, 1, 1])),
                ('css', lambda c: c.s.__setattr__('gcodes', [960])),
                ('busy', lambda c: c.s.__setattr__('inpos', False))):
            cnc=FakeCnc();cnc.s.spindle[0]['enabled']=True;setup(cnc)
            self.assertEqual(MdiExecutor(cnc).execute(
                {'action':'stop','rpm':0,'axis':'-','wcs':2}), 'ok', reason)
            self.assertEqual(cnc.blocks,['M5 S0'], reason)
        # Program, pause and machine off still reject Stop.
        for reason, setup in (
                ('program', lambda c: c.s.__setattr__('task_mode', c.MODE_AUTO)),
                ('paused', lambda c: c.s.__setattr__('task_paused', True)),
                ('off', lambda c: c.s.__setattr__('enabled', False))):
            cnc=FakeCnc();cnc.s.spindle[0]['enabled']=True;setup(cnc)
            self.assertEqual(MdiExecutor(cnc).execute(
                {'action':'stop','rpm':0,'axis':'-','wcs':2}), reason)
            self.assertEqual(cnc.blocks,[])

    def test_program_start_race_is_rejected_before_mdi(self):
        cnc=FakeCnc();cnc.race=True
        self.assertEqual(MdiExecutor(cnc).execute({'action':'start','rpm':5000,'axis':'-','wcs':2}),'program')
        self.assertEqual(cnc.blocks,[])
        self.assertFalse(cnc.s.spindle[0]['enabled'])

    def test_changed_work_system_rejected_without_mode_or_command(self):
        cnc=FakeCnc()
        self.assertEqual(MdiExecutor(cnc).execute({'action':'zero','rpm':0,'axis':'xyz','wcs':1}),'wcs')
        self.assertEqual(cnc.s.task_mode,cnc.MODE_MANUAL)
        self.assertEqual(cnc.blocks,[])

    def test_leftover_own_mdi_recovers_before_the_block(self):
        cnc=FakeCnc();executor=MdiExecutor(cnc)
        cnc.s.task_mode=cnc.MODE_MDI
        self.assertEqual(executor.execute({'action':'stop','rpm':0,'axis':'-','wcs':2}),'ok')
        self.assertEqual(cnc.blocks,['M5 S0'])
        self.assertFalse(cnc.s.spindle[0]['enabled'])
        self.assertEqual(cnc.s.task_mode,cnc.MODE_MANUAL)
        self.assertFalse(executor.left_in_mdi)

    def test_busy_mdi_is_rejected_without_deviating_the_mode(self):
        cnc=FakeCnc();executor=MdiExecutor(cnc)
        cnc.s.task_mode=cnc.MODE_MDI
        cnc.s.interp_state=3  # paused: someone else's activity inside MDI
        self.assertEqual(executor.execute({'action':'stop','rpm':0,'axis':'-','wcs':2}),'paused')
        self.assertEqual(cnc.blocks,[])
        self.assertEqual(cnc.s.task_mode,cnc.MODE_MDI)


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.now=10.
        self.config=Config(interface='wlan0',screen_id='001122334455',control_key='01'*32)
        self.cnc=FakeCnc()
        self.cnc.s.spindle[0]['enabled']=True  # test 'speed' with the spindle running
        self.control=Control(self.config,'1234567890abcdef',MdiExecutor(self.cnc),clock=lambda:self.now)
        self.ui=self.control.update({'state':'manual','wcs':2,'work':[1.,2.,3.],'selected':1500},'fedcba0987654321')

    def tearDown(self): self.control.close()

    def wait_mode(self, cnc, mode, timeout=1.0):
        # Background recovery runs in a thread, the loop never blocks.
        end=time.monotonic()+timeout
        while time.monotonic()<end and cnc.s.task_mode!=mode:
            time.sleep(0.005)
        return cnc.s.task_mode==mode
    def request(self,**changes):
        r=dict(v=3,type='command',machine='emco-f1',screen='001122334455',
               session='1234567890abcdef',nonce='fedcba0987654321',id='1111111111111111',
               token=self.ui['token'],action='speed',rpm=2000,axis='-',wcs=2)
        r.update(changes);r['mac']=signature(r,self.config.control_key)
        return r
    def encoded(self,r): return json.dumps(r).encode()

    def test_signed_request_result_and_duplicate_execute_once(self):
        r=self.request();packet=self.encoded(r)
        ack=self.control.receive(packet)
        self.assertEqual(ack['result'],'pending')
        self.assertEqual(ack['mac'],signature(ack,self.config.control_key))
        self.assertEqual(self.control.receive(packet),ack)
        final=None
        for _ in range(100):
            final=self.control.poll()
            if final: break
            time.sleep(.001)
        self.assertEqual(final['result'],'ok')
        self.assertEqual(self.control.receive(packet),final)
        self.assertEqual(self.cnc.blocks,['S2000'])

    def test_tampered_overspeed_wrong_nonce_and_expired_request(self):
        for r in [dict(self.request(),rpm=5000),self.request(rpm=5001),
                  self.request(nonce='0000000000000000')]:
            self.assertIsNone(self.control.receive(self.encoded(r)))
        self.now+=2
        self.assertEqual(self.control.receive(self.encoded(self.request()))['result'],'expired')
        self.assertEqual(self.cnc.blocks,[])

    def test_program_and_pause_rejected_on_host_even_with_valid_signature(self):
        for state in ('program','paused'):
            self.ui=self.control.update({'state':state,'wcs':2},'fedcba0987654321')
            r=self.request(id=('2' if state=='program' else '3')*16)
            self.assertEqual(self.control.receive(self.encoded(r))['result'],state)
        self.assertEqual(self.cnc.blocks,[])

    def test_mode_submission_recovers_and_executes(self):
        self.cnc.s.task_mode=self.cnc.MODE_MDI
        self.control.executor.left_in_mdi=True
        self.ui=self.control.update({'state':'mode','wcs':2},'fedcba0987654321')
        r=self.request(id='4'*16,action='stop',rpm=0)
        self.assertEqual(self.control.receive(self.encoded(r))['result'],'pending')
        final=None
        for _ in range(100):
            final=self.control.poll()
            if final: break
            time.sleep(.001)
        self.assertEqual(final['result'],'ok')
        self.assertEqual(self.cnc.blocks,['M5 S0'])
        self.assertEqual(self.cnc.s.task_mode,self.cnc.MODE_MANUAL)

    def test_busy_mdi_submission_recovers_nothing_and_rejects(self):
        self.cnc.s.task_mode=self.cnc.MODE_MDI
        self.cnc.s.interp_state=3
        self.control.executor.left_in_mdi=True
        self.ui=self.control.update({'state':'mode','wcs':2},'fedcba0987654321')
        r=self.request(id='5'*16,action='stop',rpm=0)
        self.assertEqual(self.control.receive(self.encoded(r))['result'],'pending')
        final=None
        for _ in range(100):
            final=self.control.poll()
            if final: break
            time.sleep(.001)
        self.assertEqual(final['result'],'paused')
        self.assertEqual(self.cnc.blocks,[])
        self.assertEqual(self.cnc.s.task_mode,self.cnc.MODE_MDI)

    def test_passive_auto_recover_after_quiet_mode(self):
        self.cnc.s.task_mode=self.cnc.MODE_MDI
        clock=[10.0]
        control=Control(self.config,'1234567890abcdef',MdiExecutor(self.cnc),clock=lambda:clock[0])
        try:
            for _ in range(4):
                control.update({'state':'mode','wcs':2},'fedcba0987654321');clock[0]+=1
            self.assertEqual(self.cnc.s.task_mode,self.cnc.MODE_MDI)  # 5 s not elapsed yet
            clock[0]+=1
            control.update({'state':'mode','wcs':2},'fedcba0987654321')
            self.assertTrue(self.wait_mode(self.cnc,self.cnc.MODE_MANUAL))
            self.assertEqual(self.cnc.blocks,[])
            state=control.update({'state':'manual','wcs':2},'fedcba0987654321')
            self.assertTrue(state['allowed'])
        finally: control.close()

    def test_passive_auto_recover_without_prior_commands(self):
        cnc=FakeCnc()
        cnc.s.task_mode=cnc.MODE_MDI
        clock=[20.0]
        import unittest.mock
        control=Control(self.config,'1234567890abcdef',clock=lambda:clock[0])
        try:
            self.assertIsNone(control.executor)
            with unittest.mock.patch('panel_wifi.control.MdiExecutor',
                                     return_value=MdiExecutor(cnc)) as builder:
                for _ in range(4):
                    control.update({'state':'mode','wcs':2},'fedcba0987654321');clock[0]+=1
                clock[0]+=1
                state=control.update({'state':'mode','wcs':2},'fedcba0987654321')
                self.assertTrue(self.wait_mode(cnc,cnc.MODE_MANUAL))
                self.assertEqual(builder.call_count,1)
        finally: control.close()

    def test_speed_is_stored_while_the_spindle_is_stopped(self):
        self.cnc.s.spindle[0]['enabled']=False  # spindle stopped for this test
        self.ui=self.control.update({'state':'manual','wcs':2,'selected':3000,'spindle_running':False},'fedcba0987654321')
        self.assertEqual(self.ui['selected'],3000)
        r=self.request(id='6'*16,action='speed',rpm=1200)
        self.assertEqual(self.control.receive(self.encoded(r))['result'],'pending')
        final=None
        for _ in range(100):
            final=self.control.poll()
            if final: break
            time.sleep(.001)
        self.assertEqual(final['result'],'stopped')
        self.assertEqual(self.cnc.blocks,[])
        self.ui=self.control.update({'state':'manual','wcs':2,'selected':3000,'spindle_running':False},'fedcba0987654321')
        self.assertEqual(self.ui['selected'],1200)
        self.ui=self.control.update({'state':'manual','wcs':2,'selected':3000,'spindle_running':True},'fedcba0987654321')
        self.assertEqual(self.ui['selected'],3000)

    def test_speed_with_real_rotation_passes_the_admission_snapshot(self):
        self.cnc.s.spindle[0]['enabled']=False
        self.ui=self.control.update({'state':'manual','wcs':2,'selected':1500,
                                     'spindle_running':True},'fedcba0987654321')
        r=self.request(id='7'*16,action='speed',rpm=2000)
        self.assertEqual(self.control.receive(self.encoded(r))['result'],'pending')
        final=None
        for _ in range(100):
            final=self.control.poll()
            if final: break
            time.sleep(.001)
        self.assertEqual(final['result'],'ok')
        self.assertEqual(self.cnc.blocks,['S2000'])

    def test_wcs_change_blocks_only_zero_piece(self):
        self.ui=self.control.update({'state':'manual','wcs':2},'fedcba0987654321')
        r=self.request(id='8'*16,action='start',rpm=1000,wcs=3)
        self.assertEqual(self.control.receive(self.encoded(r))['result'],'pending')
        final=None
        for _ in range(100):
            final=self.control.poll()
            if final: break
            time.sleep(.001)
        self.assertEqual(final['result'],'ok')
        self.assertIn('S1000 M3',self.cnc.blocks)

    def test_disabled_control_never_accepts_commands(self):
        disabled=Control(replace(self.config,control_key=''),'1234567890abcdef')
        try:
            self.assertFalse(disabled.update({'state':'manual','wcs':2},'fedcba0987654321')['allowed'])
            self.assertIsNone(disabled.receive(self.encoded(self.request())))
        finally: disabled.close()

if __name__=='__main__': unittest.main()
