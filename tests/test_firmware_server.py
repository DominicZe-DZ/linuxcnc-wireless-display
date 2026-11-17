# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Host OTA server: routes, signed manifest and byte count."""
import http.client
import hashlib
import hmac
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'host'))
from panel_wifi.firmware_server import start_server  # noqa: E402

KEY = 'ab' * 32
VERSION = '29025aa'


class ServerTests(unittest.TestCase):
    def setUp(self):
        folder = Path(tempfile.mkdtemp())
        self.folder = folder
        image = bytes(range(256)) * 300  # 76800 bytes
        (folder / 'display.bin').write_bytes(image)
        (folder / 'display.json').write_text(json.dumps(
            {'version': VERSION, 'sha256': hashlib.sha256(image).hexdigest()}), encoding='utf-8')
        self.nonce = 'fedcba0987654321'
        self.server, self.manifest = start_server('127.0.0.1', 0, folder, KEY,
                                                   lambda: self.nonce)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def get(self, path):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=5)
        connection.request('GET', path)
        response = connection.getresponse()
        body = response.read()
        connection.close()
        return response, body

    def test_signed_manifest_and_image(self):
        response, body = self.get('/manifest.json')
        self.assertEqual(response.status, 200)
        manifest = json.loads(body)
        self.assertEqual(manifest['version'], VERSION)
        body_esperado = (f"fw|{VERSION}|{manifest['size']}|{manifest['sha256']}"
                         f"|{self.nonce}")
        esperado = hmac.new(bytes.fromhex(KEY), body_esperado.encode('ascii'), hashlib.sha256).hexdigest()
        self.assertEqual(manifest['mac'], esperado)
        response, image = self.get('/firmware.bin')
        self.assertEqual(response.status, 200)
        self.assertEqual(len(image), manifest['size'])
        self.assertEqual(hashlib.sha256(image).hexdigest(), manifest['sha256'])
        response, _ = self.get('/otra-cosa')
        self.assertEqual(response.status, 404)

    def test_record_without_bytes_is_normalised(self):
        # load_record adds the real size; app.py signs the offer with it.
        self.assertEqual(self.manifest['bytes'], 76800)

    def test_out_of_contract_version_disables_the_offer(self):
        image = (self.folder / 'display.bin').read_bytes()
        (self.folder / 'display.json').write_text(json.dumps(
            {'version': '0123456789abcdef', 'sha256': hashlib.sha256(image).hexdigest()}),
            encoding='utf-8')
        server, offered = start_server('127.0.0.1', 0, self.folder, KEY, lambda: self.nonce)
        try:
            self.assertIsNone(offered)
        finally:
            server.server_close()

    def test_signature_changes_with_the_nonce(self):
        first = json.loads(self.get('/manifest.json')[1])
        self.nonce = '0123456789abcdef'
        second = json.loads(self.get('/manifest.json')[1])
        self.assertNotEqual(first['mac'], second['mac'])

    def test_without_image_starts_but_returns_404(self):
        server, offered = start_server('127.0.0.1', 0, self.folder / 'no-existe', KEY,
                                       lambda: self.nonce)
        self.assertIsNotNone(server)
        self.assertIsNone(offered)
        port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        connection.request('GET', '/manifest.json')
        response = connection.getresponse()
        response.read()
        connection.close()
        self.assertEqual(response.status, 404)
        server.shutdown()


if __name__ == '__main__':
    unittest.main()
