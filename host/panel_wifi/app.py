# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Main loop of the panel_wifi HAL component.

Bounded work per cycle, and it reconnects on its own no matter what the
display or the Wi-Fi are doing.
"""
import argparse
import hashlib
import hmac
import logging
import secrets
import signal
import threading
import subprocess
import time
from dataclasses import replace
from pathlib import Path
from .config import Config
from .cnc_status import NmlReader
from .control import Control
from .network import WifiTransport
from .protocol import encode, hello_nonce, sample_message
from .source import DemoSource, HalSource

LOG = logging.getLogger("panel_wifi")


def configure_logging(folder):
    """Logs go to runtime.log in the data folder. When LinuxCNC starts us from
    a HALFILE there is often no terminal to read stderr from. Falls back to
    the console if the file can't be opened."""
    handlers = []
    try:
        folder.mkdir(parents=True, exist_ok=True)
        handlers.append(logging.FileHandler(folder / 'runtime.log', encoding='utf-8'))
    except OSError:
        pass
    if not handlers:
        handlers.append(logging.StreamHandler())
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
                        handlers=handlers)


def start_firmware_server(config, control):
    """Serves display.bin/display.json from <data_dir>/firmware over HTTP when
    a control key is configured. Returns (server, offered record). The
    current nonce comes from the control channel (replay protection)."""
    if not config.firmware_port or not config.control_key:
        return None, None
    from .firmware_server import start_server
    store = config.data_path() / 'firmware'
    # A previous instance can take a few seconds to release the port after a
    # restart, so retry the bind instead of giving up straight away.
    for attempt in range(10):
        try:
            return start_server('', config.firmware_port, store, config.control_key,
                                lambda: control.nonce) or (None, None)
        except OSError as exc:
            if attempt == 9:
                LOG.warning("OTA server not available: %r", exc)
                return None, None
            LOG.warning("OTA port busy, retry %d/10 in 1 s", attempt + 1)
            time.sleep(1.0)


def run(config, demo=False):
    """Main loop, rate_hz times per second until SIGTERM/SIGINT:

        1. (re)create the Wi-Fi transport if needed (retry every 2 s)
        2. read up to 8 datagrams: hellos (learn the display, take its
           nonce) or control commands (answer with an ACK)
        3. if a hello arrived in the last 3 s: read HAL + NML, build the
           sample (+ ui, + signed firmware offer) and send it
        4. send the final ACK of a finished command, if any
        5. sleep until the next period (missed periods are skipped)

    Any network error closes the transport and step 1 starts again; HAL
    and LinuxCNC are never affected.
    """
    source = DemoSource() if demo else HalSource()
    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    transport = None
    retry_at = 0.0
    last_hello = 0.0
    nonce = None
    session = secrets.token_hex(8)
    sequence = 0
    control = Control(replace(config, control_key='') if demo else config, session)
    ui_last = None
    nml = None if demo else NmlReader()
    firmware_server, firmware_record = (None, None) if demo else start_firmware_server(config, control)
    if firmware_server is not None:
        # Without this thread the socket listens but nobody answers, and
        # shutdown() would hang on exit.
        threading.Thread(target=firmware_server.serve_forever, daemon=True).start()
        LOG.info("OTA server on port %d%s", config.firmware_port,
                 f", offering version {firmware_record['version']}"
                 if firmware_record else " (no image published yet)")
    period = 1 / config.rate_hz
    deadline = time.monotonic()
    try:
        while running:
            now = time.monotonic()
            if transport is None and now >= retry_at:
                try:
                    transport = WifiTransport(config, demo)
                    LOG.info("link ready on %s, waiting for the display", config.interface)
                except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
                    LOG.warning("network not available: %s, retrying in 2 s", exc)
                    retry_at = now + 2
            if transport is not None:
                try:
                    transport.check_link(now)
                    # A noisy peer can't turn this into an unbounded loop.
                    for _ in range(8):
                        received = transport.receive()
                        if received is None:
                            break
                        packet, address = received
                        candidate = hello_nonce(packet, config.machine_id, config.screen_id)
                        if candidate:
                            transport.select(address)
                            nonce, last_hello = candidate, now
                        elif address and address == transport.destination and nonce and now - last_hello <= 3:
                            ack = control.receive(packet)
                            if ack:
                                transport.send(encode(ack))
                    if nonce and now - last_hello <= 3:
                        values, valid = source.read(now)
                        if not demo:
                            # The spindle indicator follows real rotation. On a
                            # step/dir spindle the "on" signal can be off while
                            # the stepgen still outputs pulses (S word with M5
                            # modal). 0.05 rps is about 3 RPM.
                            values["spindle_on"] = bool(values["spindle_on"] or abs(values["rpm"]) > 0.05)
                            # Stepgen velocity comes in rev/s; rpm_input_scale = 60
                            # turns it into RPM here, with no extra RT code.
                            values["rpm"] *= config.rpm_input_scale
                        ui = {'state': 'unknown', 'wcs': 1, 'work': None, 'selected': 0}
                        if not demo:
                            snapshot = nml.read()
                            if snapshot is not None:
                                ui = snapshot
                                new_state = ui.get('state')
                                if new_state != ui_last and ui_last is not None:
                                    LOG.warning("ui state %s -> %s", ui_last, new_state)
                                ui_last = new_state
                        if not valid:
                            ui['state'] = 'unknown'
                        # While the spindle is stopped the "selected" speed is
                        # the one stored by the control channel. LinuxCNC keeps
                        # the old modal S word and would send the previous
                        # value back to the display.
                        ui['spindle_running'] = bool(values['spindle_on'])
                        ui = control.update(ui, nonce)
                        if firmware_record is not None and not demo:
                            # The offer is signed on EVERY sample with the
                            # current nonce; the display checks it against
                            # that nonce and rejects anything else.
                            body = (f"fw|{firmware_record['version']}|"
                                    f"{firmware_record['bytes']}|"
                                    f"{firmware_record['sha256']}|{nonce}")
                            offer_mac = hmac.new(bytes.fromhex(config.control_key),
                                                 body.encode('ascii'),
                                                 hashlib.sha256).hexdigest()
                            ui['upd'] = {'version': firmware_record['version'],
                                         'size': firmware_record['bytes'],
                                         'sha256': firmware_record['sha256'],
                                         'mac': offer_mac}
                        message = sample_message(session, nonce, sequence, values, valid, demo,
                                                 config.machine_id, config.screen_id)
                        message['ui'] = ui
                        transport.send(encode(message))
                        ack = control.poll()
                        if ack:
                            transport.send(encode(ack))
                        sequence = (sequence + 1) % (2 ** 32)
                except (OSError, ValueError, subprocess.SubprocessError) as exc:
                    LOG.warning("sending stopped: %s", exc)
                    transport.close()
                    transport, nonce = None, None
                    retry_at = now + 2
            # Skip missed periods instead of bursting to catch up.
            deadline += period
            after = time.monotonic()
            if deadline < after:
                deadline = after + period
            time.sleep(max(0, deadline - after))
    finally:
        if transport:
            transport.close()
        source.close()
        control.close()
        if nml is not None:
            nml.close()
        if firmware_server is not None:
            firmware_server.shutdown()


def main():
    parser = argparse.ArgumentParser(description="LinuxCNC wireless display sender")
    parser.add_argument("--config", required=True, help="host JSON file")
    parser.add_argument("--demo", action="store_true", help="send fake data, no HAL needed")
    args = parser.parse_args()
    try:
        config = Config.load(args.config)
        configure_logging(config.data_path())
        run(config, args.demo)
    except (OSError, ValueError, TypeError, RuntimeError) as exc:
        parser.exit(1, f"panel-wifi: {exc}\n")


if __name__ == "__main__":
    main()
