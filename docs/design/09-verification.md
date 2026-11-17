# 09 Verification

## Strategy

1. **Everything that can run on a PC, runs on a PC.** The parsers, the
   signatures, the battery model and the two screen modules have no hardware
   dependency, so they are tested on every push in GitHub Actions.
2. **The screen is tested with the real code.** `tests/render_dashboard.c`
   compiles `dashboard.c` and `pages.c` against LVGL with a memory
   framebuffer, drives it with a simulated touch input, counts flushed
   pixels and writes images (the ones in the README).
3. **Python and C must agree.** Signatures are computed in Python, the
   vectors are hard-coded in the C tests and checked against mbedTLS from
   the same ESP-IDF version that builds the firmware.
4. **What only the hardware can show is a manual test** (list below).

## Traceability

| Requirement | Verified by |
|---|---|
| FR-1, FR-2, FR-3, FR-4 | `render_dashboard.c` (values on screen, needle), manual M-2 |
| FR-5 | `render_dashboard.c` status texts, manual M-3 |
| FR-6 | `test_control.py::test_work_offsets_rotation_and_machine_units`, `render_dashboard.c` DRO |
| FR-7 | `test_control.py` zero tests, `render_dashboard.c` dialog + WCS change, manual M-6 |
| FR-8 | `test_control.py` speed/start/stop tests, `render_dashboard.c` slider, manual M-6 |
| FR-9 | `render_dashboard.c` (background tap does nothing, logo navigates) |
| FR-10 | `test_battery.c`, `test_battery_disabled.c`, manual M-7 |
| FR-11 | `test_update_manifest.c`, `test_firmware_server.py`, manual M-8 |
| FR-12 | `test_udp_linux.py` (demo over loopback) |
| FR-13, FR-14 | `test_host.py` discovery and link renewal, `test_udp_linux.py` address change, manual M-3 |
| NFR-1 | manual M-2 |
| NFR-2 | `test_telemetry.c` (stale/foreign packets rejected), manual M-3 |
| NFR-3 | `render_dashboard.c`: 25 identical samples cause zero flushes; RPM change and battery redraw stay local |
| NFR-4 | code review of `app.py` loop; manual M-5 |
| NFR-5 | `test_host.py::test_invalid_values_become_null_and_demo_is_explicit`, `test_telemetry.c` presence bits |
| NFR-6 | `.gitignore`, CI firmware built without credentials |
| NFR-7 | `confirm_boot_once` + rollback, manual M-8 |
| NFR-8 | the CI workflow itself |
| SR-1, SR-2 | `test_control.py::test_disabled_control_never_accepts_commands` |
| SR-3 | `test_host.py::test_rejects_mesa_and_bad_rates`, `test_route_and_subnet_guards`, `test_every_send_carries_wifi_interface`, `test_discovery_filters_ingress_interface_subnet_and_port`, manual M-4 |
| SR-4 | `test_control.py::test_mdi_commands_do_not_move_axes_and_speed_when_stopped_is_guarded` |
| SR-5 | `test_control.py` permission, program race, busy MDI, stop in degraded states |
| SR-6 | `test_control.py::test_tampered_overspeed_wrong_nonce_and_expired_request`, `test_signed_request_result_and_duplicate_execute_once`, `test_control.c` |
| SR-7 | `pages.c` timeout path, `render_dashboard.c` single command per tap |
| SR-8 | code review: no `loadrt`, no `addf`, only `HAL_IN` pins |
| SR-9 | `test_update_manifest.c` (nonce binding, key), `main.c` condition review, manual M-8 |

## Automatic tests

```sh
python -m unittest discover -s tests -p 'test_*.py' -v   # 49 tests
python tools/render_dashboard_native.py                  # LVGL render
```

The C unit tests are built by the workflow in `.github/workflows/checks.yml`
with cJSON and mbedTLS from the ESP-IDF 5.3.2 container. The last step of
the workflow builds the firmware and checks that the commit hash ended up
inside the binary.

Known gap: the HTTP download loop in `update.c` has no desktop test; it is
covered by building it and by manual test M-8.

## Manual tests (on the machine)

| ID | Test | Expected |
|---|---|---|
| M-1 | Flash, boot without NVS | NO WIFI, `discovery v2` line with the screen ID on the USB log |
| M-2 | Jog each axis, run spindle 0-5000, coolant on/off | Values match the GUI, needle smooth, indicators correct |
| M-3 | Switch the display off/on; stop/start `panel_wifi`; reboot the router | NO DATA within ~1 s, recovers on its own each time |
| M-4 | `tcpdump` on the Mesa interface during operation | No packets on 27180/27181/27190 |
| M-5 | `halcmd show thread` max times with and without the component, normal load | No meaningful difference |
| M-6 | With pairing key: speed, start, stop, zero X/Y/Z/XYZ in manual; then start a program and pause it | Commands work in manual; everything locked with a program running or paused |
| M-7 | Measure the cell with a multimeter; plug and unplug USB | Voltage within a few % after calibration, bolt follows plug/unplug |
| M-8 | Publish a new build, wait in manual mode; repeat with a corrupted `display.bin` | Updates and confirms; corrupted image rejected, old one keeps running |
| M-9 | Press the hard E-stop with the spindle started from the display | Machine stops as always; display shows the machine off |

Record date, commit, LinuxCNC version and results when running these;
"it built" is not the same as "it works on the machine".
