# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Optional control channel: a few authenticated commands, outside RT, and
completely off unless a private pairing key is configured.

No output pins, no shell, and no G-code taken from the network. A single
worker runs MDI blocks that are built here from a short list of actions.
Direct spindle commands are avoided on purpose: LinuxCNC would accept some
of them even while an AUTO program is running.
"""
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import json
import logging
import math
import re
import secrets
import threading
import time
from .cnc_status import CncStatus, permission

LOG = logging.getLogger("panel_wifi")
HEX = re.compile(r'^[0-9a-f]{16}$')
ACTIONS = {'start', 'stop', 'speed', 'zero'}


def canonical(message):
    """The exact bytes that get signed, identical on both sides (see
    firmware/main/control.c). A fixed field order joined with '|' instead of
    the JSON text, because JSON key order and spacing are not guaranteed.
        command: v|type|machine|screen|session|nonce|id|token|action|rpm|axis|wcs
        ack:     v|type|machine|screen|session|nonce|id|result
    """
    keys = ('v', 'type', 'machine', 'screen', 'session', 'nonce', 'id', 'token',
            'action', 'rpm', 'axis', 'wcs') if message['type'] == 'command' else (
            'v', 'type', 'machine', 'screen', 'session', 'nonce', 'id', 'result')
    return '|'.join(str(message[key]) for key in keys).encode('ascii')


def signature(message, key):
    """HMAC-SHA256 of canonical(message), key given as 64 hex chars, result
    as 64 lower case hex chars."""
    return hmac.new(bytes.fromhex(key), canonical(message), hashlib.sha256).hexdigest()


class MdiExecutor:
    """Checks everything again right before MDI; LinuxCNC needs MDI mode to
    run the block.

    AUTO is never interrupted to get permission. If another client starts a
    program, task rejects the switch to MDI. We don't force manual mode when
    someone else is active, because changing mode can abort their commands.
    An idle MDI (nothing executing, queues empty, in position) is switched
    back to manual: Probe Basic has no explicit manual mode of its own and
    the user shouldn't have to press anything to get control back.
    """
    RETURN_ATTEMPTS = 3
    RETURN_WAIT = 0.4
    # States where stopping the spindle is still safe and useful (no program,
    # no pause). Stop is attempted and checked again before the block.
    STOP_SAFE = {'override', 'home', 'css', 'busy', 'mode'}

    def __init__(self, cnc=None):
        self.reader = CncStatus(cnc)
        self.cnc = self.reader.cnc
        self.command = self.cnc.command()
        self.left_in_mdi = False

    def _return_manual(self, s):
        """Back to manual with bounded waits. True if the task ended up in manual."""
        for attempt in range(self.RETURN_ATTEMPTS):
            self.command.mode(self.cnc.MODE_MANUAL)
            if self.command.wait_complete(2) != self.cnc.RCS_DONE:
                time.sleep(self.RETURN_WAIT)
                continue
            s.poll()
            if s.task_mode == self.cnc.MODE_MANUAL and s.interp_state == self.cnc.INTERP_IDLE:
                self.left_in_mdi = False
                return True
            time.sleep(self.RETURN_WAIT)
        s.poll()
        if s.task_mode == self.cnc.MODE_MDI:
            self.left_in_mdi = True
            LOG.warning("task left in MDI, will try to recover")
        return False

    def _abort_mdi(self, s, result):
        """After entering MDI, switch back if the task is idle."""
        try:
            s.poll()
            if (s.task_mode == self.cnc.MODE_MDI
                    and permission(s, self.cnc, self.cnc.MODE_MDI) == 'manual'):
                self._return_manual(s)
                s.poll()
            self.left_in_mdi = s.task_mode == self.cnc.MODE_MDI
        except Exception as exc:  # noqa: BLE001 keep the real result
            LOG.warning("could not restore mode after %s: %r", result, exc)
        return result

    def _recover_idle_mdi(self, s):
        """Switch an idle MDI back to manual.

        Being stuck in MDI after one of our own commands is an artefact, not a
        real machine state: Probe Basic has no explicit manual mode and with
        no program running the task should be in manual. Only acts when the
        task is idle (same conditions a block would need: nothing executing,
        queues empty, in position, no pause, no E-stop). With a program, a
        pause or any activity, nothing is touched.
        """
        if s.task_mode != self.cnc.MODE_MDI:
            return
        if permission(s, self.cnc, self.cnc.MODE_MDI) != 'manual':
            LOG.warning("MDI busy, leaving the mode alone")
            return
        LOG.warning("idle MDI with no program, switching back to manual")
        self._return_manual(s)
        s.poll()

    def execute(self, request, live_running=None):
        """Runs one admitted request and returns its result word.

        check state -> MDI mode -> check again -> one MDI block -> check the
        effect -> back to manual. Any surprise on the way aborts without
        forcing anything. `live_running` returns the real spindle rotation
        from HAL (may differ from the modal M3/M5 on a step/dir spindle).
        """
        s, cnc, command = self.reader.stat, self.cnc, self.command

        def live_spindle():
            if live_running is not None:
                return bool(live_running())
            return bool(request.get('_spindle_running'))
        deadline = time.monotonic() + request.get('_valid_for', 1.0)
        s.poll()
        self._recover_idle_mdi(s)
        action, rpm = request['action'], request['rpm']
        reason = permission(s, cnc)
        if reason != 'manual' and not (action == 'stop' and reason in self.STOP_SAFE):
            return reason
        # spindle[0]['enabled'] is the modal M3/M4. Real rotation comes from HAL
        # (spindle velocity feedback). On a step/dir spindle a bare S word can
        # turn the motor with enabled still FALSE, so look at both.
        was_running = bool(s.spindle[0]['enabled']) or live_spindle()
        if request['action'] == 'zero' and request['wcs'] != s.g5x_index:
            return 'wcs'
        # A single block with no motion. Offsets are never computed in Python.
        if action == 'speed' and rpm != 0 and not was_running:
            # On a step/dir spindle whose drive enable doesn't follow
            # spindle.0.on, a bare S word with the spindle off would start
            # the stepgen. Don't send it: Start carries its own S word, so the
            # value is just stored and shown on the display.
            LOG.info("speed %d with spindle stopped, stored only", request['rpm'])
            return 'stopped'
        if action == 'zero':
            axes = 'XYZ' if request['axis'] == 'xyz' else request['axis'].upper()
            block = f"G10 L20 P{request['wcs']} " + ' '.join(axis + '0' for axis in axes)
        elif action == 'stop' or rpm == 0:
            block = 'M5 S0'
        else:
            block = f'S{rpm}' + (' M3' if action == 'start' else '')
        if time.monotonic() >= deadline:
            # Never enter MDI with an expired permission. It used to leave the
            # task stuck in MDI without anything sent.
            LOG.warning("permission expired before MDI, block not sent")
            return 'expired'
        LOG.info("MDI %s (%s)", action, block)
        command.mode(cnc.MODE_MDI)
        if command.wait_complete(2) != cnc.RCS_DONE:
            LOG.warning("switch to MDI not confirmed")
            return self._abort_mdi(s, 'timeout')
        s.poll()
        reason = permission(s, cnc, cnc.MODE_MDI)
        if reason != 'manual' and not (action == 'stop' and reason in self.STOP_SAFE):
            LOG.warning("state changed while in MDI (%s)", reason)
            return self._abort_mdi(s, reason)
        if action == 'zero' and request['wcs'] != s.g5x_index:
            return self._abort_mdi(s, 'wcs')
        # Race seen on a real machine: if the spindle was stopped between
        # admission and the block (someone pressed Stop elsewhere), a bare S
        # word would start it again.
        if action == 'speed' and rpm != 0:
            s.poll()
            if not live_spindle() and not bool(s.spindle[0]['enabled']):
                LOG.info("speed %d: spindle stopped before the block", rpm)
                return self._abort_mdi(s, 'stopped')
        command.mdi(block)
        if command.wait_complete(2) != cnc.RCS_DONE:
            LOG.warning("block %s not confirmed", block)
            return self._abort_mdi(s, 'timeout')
        s.poll()
        post = permission(s, cnc, cnc.MODE_MDI)
        if post != 'manual' and not (action == 'stop' and post in self.STOP_SAFE):
            LOG.warning("state changed after block %s (%s)", block, post)
            return self._abort_mdi(s, 'changed')
        result = None
        if action == 'zero':
            work = self.reader.read()['work']
            indices = range(3) if request['axis'] == 'xyz' else ['xyz'.index(request['axis'])]
            if any(abs(work[i]) > 0.001 for i in indices):
                result = 'unconfirmed'
        elif action == 'start' and rpm > 0 and not s.spindle[0]['enabled']:
            result = 'unconfirmed'
        elif action == 'stop' or rpm == 0:
            # M5 S0 must leave the modal S at zero; real rotation is seen in HAL.
            if s.spindle[0]['enabled'] or abs(s.settings[2]) > 0.5:
                result = 'unconfirmed'
        elif abs(abs(s.settings[2]) - rpm) > 0.5:
            result = 'unconfirmed'
        if not self._return_manual(s):
            LOG.warning("return to manual not confirmed after %s %s", action, block)
            return result or 'changed'
        s.poll()
        if s.task_mode != cnc.MODE_MANUAL:
            return result or 'changed'
        if action == 'zero':
            if s.g5x_index != request['wcs']:
                return 'wcs'
            work = self.reader.read()['work']
            indices = range(3) if request['axis'] == 'xyz' else ['xyz'.index(request['axis'])]
            if any(abs(work[i]) > 0.001 for i in indices):
                return 'unconfirmed'
        else:
            # Start and Stop are confirmed by the modal M3/M5; speed was already
            # confirmed through settings[2]. With a bare S word enabled stays
            # FALSE, so requiring it would report a false "unconfirmed".
            if action == 'start' and rpm > 0 and not s.spindle[0]['enabled']:
                return 'unconfirmed'
            if (action == 'stop' or rpm == 0) and s.spindle[0]['enabled']:
                return 'unconfirmed'
        return result or 'ok'


class Control:
    """Control channel state on the PC side. Used from the main loop only;
    the actual LinuxCNC work happens in a single worker thread (self.pool)
    so the 25 Hz telemetry never waits for it.

    Lifecycle of a command:
        update()  every sample: issues a 1 s token tied to (nonce, state, wcs)
        receive() datagram from the display: verify, de-duplicate, admit,
                  answer 'pending' (or a rejection) at once
        poll()    every sample: when the worker is done, return the final ACK
    Without a control key all three are no-ops and nothing is ever executed.
    """
    def __init__(self, config, session, executor=None, clock=time.monotonic):
        self.config, self.session, self.clock = config, session, clock
        self.key = config.control_key
        self.executor = executor
        self.executor_lock = threading.Lock()
        self.pool = ThreadPoolExecutor(max_workers=1) if self.key else None
        self.token, self.expires, self.fingerprint = '', 0, None
        self.records = OrderedDict()
        self.future = self.pending = None
        self.nonce = None
        self.selected = 0
        self.requested = None
        self.spindle_running = None  # real rotation from HAL, set by app.py
        self.mode_since = None
        self.recovery_attempts = 0
        self.recovering = None

    def update(self, state, nonce):
        """Adds token / allowed (and the stored speed) to the ui dict of the
        next sample. Returns the dict to send."""
        # A permission token is short lived and tied to one session and one WCS.
        now = self.clock()
        fingerprint = (nonce, state.get('state'), state.get('wcs'))
        if fingerprint != self.fingerprint or now >= self.expires:
            self.token, self.expires = secrets.token_hex(8), now + 1.0
            self.fingerprint = fingerprint
        self.nonce = nonce
        if 'spindle_running' in state:
            self.spindle_running = bool(state['spindle_running'])
        result = dict(state)
        result.update(token=self.token, allowed=bool(self.key and state.get('state') == 'manual' and not self.future))
        if self.requested is not None and not state.get('spindle_running'):
            # Our own stored speed while the spindle is stopped. LinuxCNC's
            # modal S keeps the old value and would show a number nobody chose.
            result['selected'] = self.requested
        if self.future:
            result.update(state='remote', allowed=False)
        self.auto_recover(state.get('state'), now)
        return result

    def auto_recover(self, state, now):
        """If the task sits in an idle MDI, switch it back to manual without
        waiting for a command. Nothing happens with a program, a pause or any
        activity. The executor is only created on the first attempt, so just
        starting the sender never opens a command channel to LinuxCNC."""
        if not self.key or self.future is not None or self.recovering is not None:
            return
        if state == 'mode':
            if self.mode_since is None:
                self.mode_since = now
                return
            if now - self.mode_since < 5.0 or self.recovery_attempts >= 3:
                return
            self.recovery_attempts += 1
            self.mode_since = now  # another 5 s before the next try
            self.recovering = True
            # In its own thread: _return_manual can take seconds and the 25 Hz
            # loop must keep sending (otherwise the display renews its nonce
            # and the ACK of a command that did run gets lost).
            threading.Thread(target=self._recover_async, daemon=True).start()
            return
        self.mode_since = None
        self.recovery_attempts = 0

    def _recover_async(self):
        try:
            with self.executor_lock:
                if self.executor is None:
                    self.executor = MdiExecutor()
                s = self.executor.reader.stat
                s.poll()
                if s.task_mode == self.executor.cnc.MODE_MDI:
                    self.executor._recover_idle_mdi(s)
        except Exception as exc:  # noqa: BLE001 recovery must never kill the loop
            LOG.warning("background recovery failed: %r", exc)
        finally:
            self.recovering = None

    def receive(self, packet):
        """Each ID runs once at most. A repeated request gets the same ACK back."""
        if not self.key:
            return None
        try:
            r = json.loads(packet)
            if (not isinstance(r, dict) or r.get('v') != 3 or r.get('type') != 'command'
                    or r.get('machine') != self.config.machine_id or r.get('screen') != self.config.screen_id
                    or r.get('session') != self.session or r.get('nonce') != self.nonce
                    or not isinstance(r.get('id'), str) or not HEX.fullmatch(r['id'])
                    or r.get('action') not in ACTIONS or r.get('axis') not in ('x', 'y', 'z', 'xyz', '-')
                    or type(r.get('rpm')) is not int or not 0 <= r['rpm'] <= 5000
                    or type(r.get('wcs')) is not int or not 1 <= r['wcs'] <= 9
                    or not isinstance(r.get('mac'), str)
                    or not hmac.compare_digest(r['mac'], signature(r, self.key))):
                return None
        except (ValueError, KeyError, TypeError, UnicodeError):
            return None
        identity = (r['nonce'], r['id'])
        digest = signature(r, self.key)
        if identity in self.records:
            prior_digest, ack = self.records[identity]
            return ack if prior_digest == digest else None
        if len(self.records) >= 128:
            self.records.popitem(last=False)
        result = 'pending'
        if self.future:
            result = 'busy'
        elif r.get('token') != self.token or self.clock() >= self.expires:
            result = 'expired'
        else:
            state_now = self.fingerprint[1]
            # 'mode' is accepted too: the worker switches an idle task back to
            # manual and checks again before the block, or answers why not.
            if state_now != 'manual' and state_now != 'mode':
                result = state_now
        if result == 'pending' and r['action'] == 'zero' and r['wcs'] != self.fingerprint[2]:
            result = 'wcs'
        if result == 'pending' and r['action'] == 'zero' and r['axis'] == '-':
            result = 'invalid'
        if result in ('pending', 'stopped') and r['action'] in ('start', 'speed'):
            # Remember the speed even if no block runs: it is what the display
            # shows until the next Start.
            self.requested = r['rpm']
        ack = self.ack(r, result)
        self.records[identity] = (digest, ack)
        if result == 'pending':
            # The time budget doesn't depend on what's left of the token: the
            # permission was checked at admission. Without this margin a token
            # expiring halfway left the task in the middle of an MDI.
            r['_deadline'] = max(self.clock() + 0.70, self.expires)
            r['_started'] = self.clock()
            # Snapshot of the real rotation at admission, so the worker doesn't
            # see it change halfway through.
            r['_spindle_running'] = self.spindle_running
            self.pending = r
            self.future = self.pool.submit(self.execute, r)
        return ack

    def execute(self, r):
        try:
            with self.executor_lock:
                if self.executor is None:
                    self.executor = MdiExecutor()
                if self.clock() >= r['_deadline']:
                    return 'expired'
                r['_valid_for'] = r['_deadline'] - self.clock()
                return self.executor.execute(
                    r, live_running=lambda: self.spindle_running)
        except Exception as exc:
            # Never retry, and never turn an executor failure into a command.
            LOG.warning("control request failed: %r", exc)
            if self.executor is not None:
                try:
                    state = self.executor.reader.stat
                    state.poll()
                    if state.task_mode == self.executor.cnc.MODE_MDI:
                        self.executor.left_in_mdi = True
                except Exception:
                    pass
            return 'error'

    def ack(self, r, result):
        """Signed v3 ACK for request r with the given result word."""
        ack = {k: r[k] for k in ('machine', 'screen', 'session', 'nonce', 'id')}
        ack.update(v=3, type='ack', result=result)
        ack['mac'] = signature(ack, self.key)
        return ack

    def poll(self):
        """Final ACK of the finished command, or None if nothing finished."""
        if self.future is None or not self.future.done():
            return None
        r = self.pending
        result = self.future.result()
        spent = (self.clock() - r['_started']) * 1000.0
        LOG.info("control %s %s axis %s: %s (%.0f ms after the request)",
                 r['action'], r['rpm'], r['axis'], result, spent)
        ack = self.ack(r, result)
        self.records[(r['nonce'], r['id'])] = (signature(r, self.key), ack)
        self.future = self.pending = None
        self.expires = 0  # fresh permission after every transaction
        return ack

    def close(self):
        if self.pool:
            self.pool.shutdown(wait=True, cancel_futures=True)
