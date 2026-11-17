# Changelog

## 1.0.0 - 2026-11-17

First public release.

- `panel_wifi` userspace HAL component: RPM, feed, XYZ, spindle and coolant
  over UDP, pinned to the Wi-Fi interface, discovery without fixed IPs.
- Firmware for the Waveshare ESP32-S3-Touch-LCD-2.8C: Classic RPM gauge,
  DRO page and Spindle page, partial LVGL refresh.
- Optional control channel (spindle speed, start/stop, work zero) with
  HMAC-SHA256, disabled unless a pairing key is set on both sides.
- Signed OTA updates served by the LinuxCNC PC, with rollback.
- Battery indicator for the on-board LiPo connector.
- 3D printed enclosure (STEP, Fusion, 3MF, STL) under CC BY 4.0.
