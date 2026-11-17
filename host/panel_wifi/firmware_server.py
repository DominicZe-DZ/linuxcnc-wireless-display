# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Small HTTP server that hands the display its firmware image.

Only two GET routes, /manifest.json and /firmware.bin. The image lives in the
firmware folder of the data directory (display.bin + display.json). The
manifest is signed on EVERY request with the pairing key and the current
nonce: 'fw|version|size|sha256|nonce', so a captured manifest is useless in
another session.
"""
import hashlib
import hmac
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def load_record(store: Path):
    """Loads display.bin + display.json. (None, None) if missing or invalid."""
    bin_path = store / 'display.bin'
    json_path = store / 'display.json'
    if not bin_path.exists() or not json_path.exists():
        return None, None
    try:
        record = json.loads(json_path.read_text(encoding='utf-8'))
    except ValueError:
        return None, None
    data = bin_path.read_bytes()
    version = record.get('version') if isinstance(record, dict) else None
    # The firmware wants a 1..15 char alphanumeric version and an image
    # between 1 KiB and 3 MiB. Anything else just disables the offer, never
    # telemetry or control.
    if (not isinstance(version, str) or not 1 <= len(version) <= 15
            or any(c not in '0123456789abcdefghijklmnopqrstuvwxyz' for c in version)
            or not 1024 <= len(data) <= 3 * 1024 * 1024
            or hashlib.sha256(data).hexdigest() != record.get('sha256')):
        return None, None
    # The real file size wins over whatever the JSON says, so an incomplete
    # record can't break the sender (app.py signs with `bytes` and `sha256`).
    return data, dict(record, bytes=len(data))


class FirmwareHandler(BaseHTTPRequestHandler):
    store: Path = None
    control_key: str = ''
    nonce_of = None  # callable returning the current nonce (16 hex) or None

    def manifest_for(self):
        data, record = load_record(self.store)
        if data is None or record is None:
            return None, None
        nonce = self.nonce_of() if self.nonce_of else ''
        if not nonce:
            return None, None
        body = (f"fw|{record['version']}|{len(data)}|{record['sha256']}|{nonce}")
        mac = hmac.new(bytes.fromhex(self.control_key), body.encode('ascii'),
                       hashlib.sha256).hexdigest()
        return {'version': record['version'], 'size': len(data),
                'sha256': record['sha256'], 'mac': mac}, data

    def do_GET(self):
        try:
            import logging
            logging.getLogger("panel_wifi").info("OTA request %s from %s",
                                                 self.path, self.client_address[0])
        except Exception:
            pass
        if self.path == '/manifest.json':
            manifest, _ = self.manifest_for()
            if manifest is None:
                self.send_response(404)
                self.end_headers()
                return
            body = json.dumps(manifest).encode('ascii')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == '/firmware.bin':
            manifest, data = self.manifest_for()
            if manifest is None:
                self.send_response(404)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        self.send_response(404)
        self.end_headers()

    def log_message(self, fmt, *args):  # keep runtime.log quiet
        return


def start_server(host: str, port: int, store: Path, control_key: str, nonce_of):
    """Returns (server, offered record), or (None, None) without a control key."""
    if not control_key:
        return None, None
    handler = type('Handler', (FirmwareHandler,),
                   {'store': store, 'control_key': control_key,
                    'nonce_of': staticmethod(nonce_of)})
    try:
        server = ThreadingHTTPServer((host, port), handler)
    except OSError as exc:
        raise OSError(f'port {port} is busy: {exc}') from exc
    server.daemon_threads = True
    data, record = load_record(store)
    return server, record
