# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Protocol and network isolation tests, no LinuxCNC needed."""
import ipaddress
import json
from pathlib import Path
import tempfile
import socket
import struct
import unittest
from unittest.mock import patch
from panel_wifi.config import Config
from panel_wifi.protocol import encode, hello_nonce, sample_message
from panel_wifi.network import inspect_link, WifiTransport, Link, IP_PKTINFO
from panel_wifi.source import HalSource


class ProtocolTests(unittest.TestCase):
    def test_hello_rejects_invalid_or_oversized_inputs(self):
        for value in (b"garbage", b"[]", b"null", b"x" * 1201,
                      b'{"v":true,"type":"hello","nonce":"0123456789abcdef"}',
                      b'{"v":1,"type":"command","nonce":"0123456789abcdef"}'):
            self.assertIsNone(hello_nonce(value, "emco-f1", "001122334455"))
        message = {"v": 2, "type": "hello", "nonce": "0123456789abcdef", "machine": "emco-f1", "screen": "001122334455"}
        self.assertEqual(hello_nonce(encode(message), "emco-f1", "001122334455"), message["nonce"])
        for changes in ({"v": 1}, {"v": True}, {"machine": "otra"}, {"screen": "ffffffffffff"}, {"nonce": "bad"}):
            self.assertIsNone(hello_nonce(encode({**message, **changes}), "emco-f1", "001122334455"))

    def test_invalid_values_become_null_and_demo_is_explicit(self):
        message = sample_message("a" * 16, "b" * 16, 1,
                                 {"rpm": float("nan"), "spindle_on": True, "x": False}, demo=True)
        self.assertIsNone(message["data"]["rpm"])
        self.assertIsNone(message["data"]["x"])
        self.assertTrue(message["data"]["spindle_on"])
        self.assertTrue(message["demo"])
        self.assertLess(len(encode(message)), 1200)

    def test_encoder_rejects_nonfinite_and_huge_payload(self):
        for value in ({"x": float("inf")}, {"x": "a" * 1300}):
            with self.assertRaises(ValueError):
                encode(value)


class ConfigTests(unittest.TestCase):
    def load(self, **changes):
        value = {"interface": "wlan0", "screen_id": "001122334455", **changes}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "config.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            return Config.load(path)

    def test_rejects_mesa_and_bad_rates(self):
        for changes in ({"screen_id": "bad"}, {"rate_hz": 0}, {"rate_hz": True},
                        {"machine_id": "a\"b"}, {"screen_port": 65536}):
            with self.assertRaises(ValueError):
                self.load(**changes)

    def test_default_rate(self):
        self.assertEqual(self.load().rate_hz, 25)

    def test_rps_conversion_configuration(self):
        self.assertEqual(self.load(rpm_input_scale=60).rpm_input_scale * 25, 1500)
        for scale in (0, -1, True, float('inf'), float('nan')):
            with self.assertRaises(ValueError):
                self.load(rpm_input_scale=scale)


class NetworkTests(unittest.TestCase):
    @patch("panel_wifi.network.sys.platform", "linux")
    @patch("panel_wifi.network.socket.if_nametoindex", return_value=7)
    @patch("panel_wifi.network.Path.is_dir", return_value=True)
    def test_route_and_subnet_guards(self, *_):
        config = Config("wlan0", "001122334455")
        def run(command, timeout):
            return json.dumps([{"addr_info": [{"local": "192.168.1.50", "prefixlen": 24}]}]
                              if "addr" in command else [{"dev": "wlan0"}]).encode()
        self.assertEqual(inspect_link(config, run=run), Link("192.168.1.50", ipaddress.IPv4Network("192.168.1.0/24"), 7))
        overlapping = Config("wlan0", "001122334455")
        with self.assertRaises(ValueError):
            inspect_link(overlapping, run=lambda *a, **kw: b'[{"addr_info":[{"local":"10.10.10.1","prefixlen":24}]}]')

    @patch("panel_wifi.network.inspect_link", return_value=Link("192.168.1.50", ipaddress.IPv4Network("192.168.1.0/24"), 7))
    @patch("panel_wifi.network.socket.socket")
    def test_every_send_carries_wifi_interface(self, factory, _inspect):
        transport = WifiTransport(Config("wlan0", "001122334455"))
        transport.select(("192.168.1.80", 27181))
        transport.send(b"sample")
        args = factory.return_value.sendmsg.call_args.args
        self.assertEqual(int.from_bytes(args[1][0][2][:4], __import__('sys').byteorder), 7)
        self.assertEqual(args[3], ("192.168.1.80", 27181))
        transport.close()

    @patch("panel_wifi.network.sys.platform", "linux")
    @patch("panel_wifi.network.Path.is_dir", return_value=False)
    def test_non_wifi_refused_outside_demo(self, _):
        with self.assertRaises(ValueError):
            inspect_link(Config("eth0", "192.168.1.80"))

    @patch("panel_wifi.network.inspect_link", return_value=Link("192.168.1.50", ipaddress.IPv4Network("192.168.1.0/24"), 7))
    @patch("panel_wifi.network.socket.socket")
    def test_discovery_filters_ingress_interface_subnet_and_port(self, factory, _inspect):
        transport = WifiTransport(Config("wlan0", "001122334455"))
        for index, address, accepted in [(7, ("192.168.1.80", 27181), True),
                                          (8, ("192.168.1.80", 27181), False),
                                          (7, ("10.10.10.10", 27181), False),
                                          (7, ("192.168.1.80", 12345), False),
                                          (7, ("192.168.1.255", 27181), False)]:
            ancillary = [(socket.IPPROTO_IP, IP_PKTINFO, struct.pack("=I4s4s", index, b'\0'*4, b'\0'*4))]
            factory.return_value.recvmsg.return_value = (b'hello', ancillary, 0, address)
            self.assertEqual(transport.receive(), (b'hello', address) if accepted else (b'', None))
        transport.close()

    @patch("panel_wifi.network.socket.socket")
    def test_host_address_change_forces_link_renewal(self, _factory):
        old = Link("192.168.1.50", ipaddress.IPv4Network("192.168.1.0/24"), 7)
        new = Link("192.168.1.51", old.network, 7)
        with patch("panel_wifi.network.inspect_link", side_effect=[old, old, new]):
            transport = WifiTransport(Config("wlan0", "001122334455"))
            transport.check_link(0)
            with self.assertRaises(OSError):
                transport.check_link(2)
            transport.close()


class SourceTests(unittest.TestCase):
    def test_liquid_or_mist_in_userspace(self):
        source = object.__new__(HalSource)
        source.component = {"rpm": 0, "feed": 0, "x": 0, "y": 0, "z": 0,
                            "spindle_on": False, "coolant": False, "coolant_mist": False, "enabled": True}
        for liquid, mist in ((False, False), (True, False), (False, True), (True, True)):
            source.component.update(coolant=liquid, coolant_mist=mist)
            values, valid = source.read(0)
            self.assertEqual(values["coolant"], liquid or mist)
            self.assertTrue(valid)


if __name__ == "__main__":
    unittest.main()
