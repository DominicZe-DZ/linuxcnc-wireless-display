# Development

## Path of one value

1. A HAL `net` line connects an existing signal to a `panel_wifi` input.
2. `source.py` reads the pin. It never writes it.
3. `app.py` wakes up at `rate_hz` and builds a snapshot.
4. `cnc_status.py` adds the NML part (state, WCS, work coordinates).
5. `protocol.py` normalises types and encodes the JSON.
6. `network.py` sends it on the Wi-Fi interface with `IP_PKTINFO`.
7. `main.c` receives it and checks source, nonce, session and sequence.
8. `telemetry.c` turns it into a `panel_sample_t`.
9. `dashboard.c` and `pages.c` update the LVGL objects that changed.

Keep it that way: no HAL reads in `network.py`, no LVGL calls outside the
main loop, no machine knowledge in the transport. The C parser has no
hardware dependency so it can be tested with a desktop compiler.

## Tests

Python (any OS, the UDP integration test only runs on Linux):

```sh
python -m pip install -e .
python -m unittest discover -s tests -p 'test_*.py' -v
```

| File | Covers |
|---|---|
| `test_host.py` | Config validation, discovery filters, Mesa/subnet guards, `IP_PKTINFO` on every send |
| `test_control.py` | Permissions, zeroing, spindle guards, signatures, replay, expiry, MDI recovery |
| `test_firmware_server.py` | Signed OTA manifest, `Content-Length`, record checks |
| `test_version.py` | Build ID from git / `GITHUB_SHA` / CI error |
| `test_machine_demo.py` | Demo G-code envelope and states |
| `test_udp_linux.py` | Real datagrams over loopback and clean SIGTERM |

C (these need cJSON and mbedTLS from ESP-IDF; see the workflow for the exact
commands):

- `test_telemetry.c`: parser contract, truncation, nonce, sequence wrap, `ui`.
- `test_control.c`: HMAC matches the Python vectors, ACK checks.
- `test_update_manifest.c`: OTA manifest, one signed body per case.
- `test_battery.c` / `test_battery_disabled.c`: battery model and the
  disabled build.
- `render_dashboard.c`: runs the real pages on LVGL in memory. Checks that
  identical samples draw nothing, the touch gestures, the slider, the
  program lock, and writes the images used in these docs:

  ```sh
  python tools/render_dashboard_native.py
  # images in .cache/native-preview/*.ppm
  ```

GitHub Actions runs the Python tests first, then the C tests, the LVGL
render and the ESP32-S3 build. The firmware job checks that the commit hash
really ended up inside the binary.

Known gap: the OTA download state machine in `update.c` is only tested by
building it and on real hardware.

## Adding a value

Say you want the PC temperature on screen. Decide the unit, where it comes
from, how you know it's valid, and how often it needs updating, before
writing code.

1. Add it on the Python side: a HAL pin, or a cached reading if it comes
   from Linux. A temperature doesn't need 25 reads per second, and never
   start a process per sample.
2. Add the field and its presence bit to `telemetry.h` / `telemetry.c`.
3. Show dashes when it is missing.
4. Extend the tests and [PROTOCOL.md](PROTOCOL.md). Stay under 1200 bytes.
5. Never change the unit of an existing field without bumping the protocol
   version.

## Regenerating assets

All generated files are committed, so building the firmware only needs
ESP-IDF.

| Script | Output | Needs |
|---|---|---|
| `tools/generate_dashboard_assets.py` | `dashboard_assets.h` (logos, indicators, needle) | Pillow, Node + sharp |
| `tools/generate_header_assets.py` | `header_assets.h` (small logos for DRO/Spindle) | Pillow |
| `tools/generate_dz_logo.py` | `dz_logo.h` | Pillow |
| `tools/generate_battery_assets.py` | `battery_assets.h` | Pillow |
| `tools/generate_fonts.sh` | `font_*.c` | Node (installs lv_font_conv) |

`prepare_firmware.py` pins the Waveshare zip by SHA-256. If it changes,
don't just update the hash: check what changed in the drivers, the LVGL
version and the IDF APIs first.

## Design document PDF

The chapters in `docs/design` can be printed as one PDF (cover, contents,
diagrams, page numbers). Needs Node 18+ and Chrome or Chromium:

```sh
cd tools/design-pdf
npm install
node build.mjs --version 1.0.0
```

The PDF is written to the repository root (ignored by Git) and attached to
the GitHub release, so the Markdown stays the only source.

## Demo program

`tools/generate_machine_demo.py` writes an air-cutting program that moves X,
Y and Z together on a closed path, steps the spindle through 1000, 2500,
5000 and 0 RPM, and cycles coolant off / flood / mist / both / off. It only
writes a file.

```sh
python tools/generate_machine_demo.py --half-x 35 --half-y 30 --half-z 35 \
    --feed 500 --cycles 4 --output gcode/display_demo_f1.ngc
```

The `--half-*` values are half travels around the **current G54 zero**, so
that example covers 70 x 60 x 70 mm. A JSON next to the program lists the
envelope and the sequence. The program doesn't touch offsets, tools or HAL,
and ends with spindle and coolant off.

Before running it, on the LinuxCNC PC with the machine homed and G54
active:

```sh
python3 tools/check_machine_demo.py --ini ~/linuxcnc/configs/YOUR_CONFIG/YOUR.ini gcode/display_demo_f1.ngc
```

It only reads the INI and `linuxcnc.stat` and checks every point against
your soft limits with the current G54. It knows nothing about vices, parts
or tools, so look at the preview and keep the volume clear.
