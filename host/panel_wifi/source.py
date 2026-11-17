# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Data sources. Importing this module doesn't require LinuxCNC."""
import math
from .protocol import CHANNELS


class HalSource:
    """Creates the 'panel_wifi' userspace HAL component.

    Pins (all HAL_IN, the component never writes anything):
        panel_wifi.rpm          float  spindle speed (scaled later)
        panel_wifi.feed         float  current velocity, units/s
        panel_wifi.x / y / z    float  machine position
        panel_wifi.spindle_on   bit
        panel_wifi.coolant      bit    flood
        panel_wifi.coolant_mist bit    mist (OR-ed with coolant)
        panel_wifi.enabled      bit    set last in the HAL file
    """
    def __init__(self):
        import hal
        self.component = hal.component("panel_wifi")
        for name, kind in CHANNELS.items():
            self.component.newpin(name, hal.HAL_BIT if kind == "bit" else hal.HAL_FLOAT, hal.HAL_IN)
        # The HAL file sets 'enabled' AFTER all the inputs are connected.
        self.component.newpin("coolant_mist", hal.HAL_BIT, hal.HAL_IN)
        self.component.newpin("enabled", hal.HAL_BIT, hal.HAL_IN)
        self.component.ready()  # never wait for the network before ready()

    def read(self, now):
        """Returns (values, enabled). Pins are read one by one, so this is not
        an atomic copy of one servo period; fine for a display."""
        values = {key: self.component[key] for key in CHANNELS}
        values["coolant"] = bool(values["coolant"] or self.component["coolant_mist"])
        return values, bool(self.component["enabled"])

    def close(self):
        self.component.exit()


class DemoSource:
    """Fake data flagged as DEMO. Doesn't import hal or touch the machine."""
    def read(self, now):
        return ({"rpm": 2000 + 1600 * math.sin(now / 4), "feed": 650 / 60,
                 "x": 125 + 30 * math.sin(now / 8), "y": -75, "z": -120,
                 "spindle_on": True, "coolant": True}, True)

    def close(self):
        pass
