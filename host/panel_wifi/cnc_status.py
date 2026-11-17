# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""LinuxCNC status read from user space. Nothing in here sends commands.

Work coordinates are computed from the offsets and rotation reported by
LinuxCNC. Setting a zero is a different job and is left to the interpreter
with G10 L20 (see control.py).
"""
import logging
import math
import threading
import time

LOG = logging.getLogger("panel_wifi")


def work_position(status):
    """Work coordinates (X, Y, Z in mm) the same way LinuxCNC's own DRO
    computes them, from a polled linuxcnc.stat:

        p      = actual_position - g5x_offset - tool_offset   (machine units)
        p.xy   = rotate(p.xy, -rotation_xy)                   (G10 L2 R...)
        work   = (p - g92_offset) / linear_units              (-> mm)

    The rotation undoes the XY rotation of the active work system:
        x' = x cos(a) - y sin(a)
        y' = x sin(a) + y cos(a)      with a = -rotation_xy in radians
    linear_units is machine units per mm (1.0 for a metric machine,
    1/25.4 for an inch one), so dividing converts to mm.

    Raises ValueError on nonsense instead of sending a wrong number.
    """
    xyz = [status.actual_position[i] - status.g5x_offset[i] - status.tool_offset[i] for i in range(3)]
    angle = math.radians(-status.rotation_xy)
    x, y = xyz[:2]
    xyz[:2] = [x * math.cos(angle) - y * math.sin(angle),
               x * math.sin(angle) + y * math.cos(angle)]
    scale = status.linear_units
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError('invalid machine units')
    result = [(xyz[i] - status.g92_offset[i]) / scale for i in range(3)]
    if not all(math.isfinite(v) and abs(v) <= 1e7 for v in result):
        raise ValueError('invalid coordinates')
    return result


def permission(s, cnc, mode=None):
    """Classifies the LinuxCNC state into one word, checked in this order
    (first match wins):

        paused    task or interpreter paused
        program   AUTO mode (a program is loaded/running)
        off       machine off, E-stop, or motion not enabled
        mode      not in the expected mode (MANUAL, or `mode` if given)
        busy      interpreter/exec not idle, queues not empty, not in
                  position or still moving
        home      some joint not homed
        css       G96 active (gcodes holds G-codes x10, so 960 = G96)
        override  spindle override not exactly 100 %
        manual    none of the above: idle manual, commands allowed

    Fail closed: anything unexpected ends up as something other than
    'manual'.
    """
    if s.task_paused or s.paused or s.interp_state == cnc.INTERP_PAUSED:
        return 'paused'
    if s.task_mode == cnc.MODE_AUTO:
        return 'program'
    if s.task_state != cnc.STATE_ON or s.estop or not s.enabled:
        return 'off'
    if s.task_mode != (cnc.MODE_MANUAL if mode is None else mode):
        return 'mode'
    if (s.interp_state != cnc.INTERP_IDLE or s.exec_state != cnc.EXEC_DONE
            or s.queue or s.active_queue or getattr(s, 'queued_mdi_commands', 0)
            or not s.inpos or abs(s.current_vel) > 1e-6):
        return 'busy'
    if not all(s.homed[:s.joints]):
        return 'home'
    if 960 in s.gcodes:
        return 'css'
    if abs(s.spindle[0]['override'] - 1.0) > 1e-6:
        return 'override'
    return 'manual'


class CncStatus:
    def __init__(self, cnc=None):
        if cnc is None:
            import linuxcnc as cnc
        self.cnc = cnc
        self.stat = cnc.stat()

    def read(self):
        """Polls NML and returns the 'ui' part of a sample (blocking)."""
        self.stat.poll()
        s = self.stat
        return {'state': permission(s, self.cnc), 'wcs': s.g5x_index,
                'work': work_position(s),
                'selected': max(0, min(5000, round(abs(s.settings[2]))))}


class NmlReader:
    """Reads the LinuxCNC status in its own thread and keeps the latest snapshot.

    stat.poll() can block while the task is busy, and the sender loop must
    never block. read() returns the last snapshot, or None if it is older
    than `max_age` seconds. The thread keeps retrying forever.
    """
    def __init__(self, cnc=None, max_age=2.0):
        self.max_age = max_age
        self.cnc = cnc
        self._reader = None
        self.snapshot = None
        self.stamp = 0.0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _loop(self):
        while not self._stop.is_set():
            try:
                if self._reader is None:
                    self._reader = CncStatus(self.cnc)
                self.snapshot = self._reader.read()
                self.stamp = time.monotonic()
            except Exception as exc:
                LOG.warning("NML read failed: %r, no ui data for 2 s", exc)
                self.snapshot = None
                self._reader = None
                time.sleep(2.0)
            time.sleep(0.04)

    def read(self):
        snapshot = self.snapshot
        if snapshot is None or time.monotonic() - self.stamp > self.max_age:
            return None
        return dict(snapshot)

    def close(self):
        self._stop.set()
