# Over-the-air updates

Once the display has been flashed over USB with this firmware, later
versions can be installed over Wi-Fi. The LinuxCNC PC serves the image
itself; nothing goes through the Internet.

OTA needs the [pairing key](CONTROL_CHANNEL.md#pairing-key) on both sides,
because the offer is signed with it. Without a key the OTA server doesn't
start.

## Publishing a new version

1. Build the firmware (see [FIRMWARE.md](FIRMWARE.md)). Run
   `tools/generate_version.py` first so the image carries the commit hash.
   The GitHub Actions artifact works too.
2. Copy the `.bin` to the LinuxCNC PC and publish it:

   ```sh
   python3 tools/publish_firmware.py linuxcnc_wireless_display.bin --version 1a2b3c4
   ```

   This writes `display.bin` and `display.json` (version, size, SHA-256)
   into `local/firmware/`. Without `--version` it reads
   `firmware/main/version.h`.
3. Restart LinuxCNC so `panel_wifi` loads the new offer.

The display compares the offered version with its own build ID. When it is
different, the machine is in idle manual mode and no command is pending, it
downloads the image, checks it, switches boot partition and restarts.
Progress is shown on screen and every control is locked meanwhile.

## How it is protected

- The offer travels inside the telemetry (`ui.upd`) and its `mac` is
  HMAC-SHA256 with the pairing key over `fw|version|size|sha256|nonce`.
  Because the display's current nonce is part of it, a captured offer is
  useless in another session.
- The download is plain HTTP from the PC that is sending the current stream
  (`/firmware.bin` on port 27190). It must answer `200`, with a
  `Content-Length` equal to the signed size, no chunked encoding and no
  compression.
- The image is written to the inactive slot while a SHA-256 is computed on
  the fly. The boot partition only changes if the hash matches the signed
  one.
- The download gives up on stalls or if it gets too slow (2 s to connect,
  5 s for the header, 15 s without data, at least 6 KB/s overall). A failure
  leaves the running image untouched; the display goes back to normal and
  tries again in 10 minutes.
- The new image boots as "pending verify". It marks itself valid only when
  it receives a valid stream from the PC. If it never gets that far, the
  bootloader rolls back to the previous image.

Someone else on your LAN can at most make a download fail. They can't make
the display accept a different image without the key.

## Limits

- The image must fit in a 3 MB app slot.
- The router has to allow HTTP between Wi-Fi clients (it already allows the
  UDP traffic).
- Upgrading from a single-app partition table (older builds) to the OTA
  layout needs one last USB flash: bootloader, partition table and app.
  NVS at 0x9000 is not touched, so Wi-Fi and the key survive.
