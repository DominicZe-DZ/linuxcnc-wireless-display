# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Real IP_PKTINFO round trip on Linux loopback. Runs in CI."""
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import unittest


@unittest.skipUnless(sys.platform.startswith("linux"), "IP_PKTINFO is Linux only")
class UdpIntegration(unittest.TestCase):
    def test_real_datagrams_and_stop(self):
        with tempfile.TemporaryDirectory() as folder, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as screen:
            screen.bind(("127.0.0.2", 0))
            screen_port = screen.getsockname()[1]
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
                reservation.bind(("127.0.0.1", 0))
                host_port = reservation.getsockname()[1]
            screen.settimeout(.3)
            screen.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            config = Path(folder) / "host.json"
            config.write_text(json.dumps({"interface": "lo", "screen_id": "001122334455", "host_port": host_port, "screen_port": screen_port}))
            process = subprocess.Popen([sys.executable, "-m", "panel_wifi.app", "--demo", "--config", str(config)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                nonce = secrets.token_hex(8)
                result = None
                end = time.monotonic() + 8
                while time.monotonic() < end:
                    screen.sendto(json.dumps({"v": 2, "type": "hello", "nonce": nonce, "machine": "emco-f1", "screen": "001122334455"}).encode(), ("127.255.255.255", host_port))
                    try:
                        packet, peer = screen.recvfrom(1201)
                        result = json.loads(packet)
                        break
                    except socket.timeout:
                        if process.poll() is not None:
                            self.fail(process.stderr.read().decode())
                self.assertIsNotNone(result)
                self.assertEqual(result["nonce"], nonce)
                self.assertEqual(peer[0], "127.0.0.1")
                self.assertTrue(result["demo"])
                self.assertTrue(result["valid"])
                self.assertIn("rpm", result["data"])
                # Same screen ID, new DHCP lease: the host picks up the new
                # address from the next hello without restarting.
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as moved:
                    moved.bind(("127.0.0.3", screen_port))
                    moved.settimeout(2)
                    moved.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                    fresh_nonce = secrets.token_hex(8)
                    moved.sendto(json.dumps({"v": 2, "type": "hello", "nonce": fresh_nonce, "machine": "emco-f1", "screen": "001122334455"}).encode(), ("127.255.255.255", host_port))
                    fresh = json.loads(moved.recvfrom(1201)[0])
                    self.assertEqual(fresh["nonce"], fresh_nonce)
                    self.assertEqual(fresh["screen"], "001122334455")
            finally:
                process.terminate()
                process.wait(timeout=4)
                process.stderr.close()
            self.assertEqual(process.returncode, 0)
