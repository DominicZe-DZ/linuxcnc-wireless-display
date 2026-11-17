# Building and flashing the firmware

Board: **Waveshare ESP32-S3-Touch-LCD-2.8C** (ST7701S panel, GT911 touch,
480x480, 16 MB flash, octal PSRAM). The 2.8 and 2.8B boards are different
hardware and won't work with this build.

## 1. Get ESP-IDF and the board drivers

Install **ESP-IDF 5.3.2** following Espressif's guide and open a terminal
with the IDF environment loaded. From the repository root:

```sh
python tools/generate_version.py
python tools/prepare_firmware.py
```

`prepare_firmware.py` downloads Waveshare's official demo, checks its
SHA-256 and copies only the LCD, touch, I/O expander and I2C drivers plus
LVGL into git-ignored folders. The demo isn't redistributed here. If
Waveshare ever changes the zip, the script stops instead of importing
something unknown.

`generate_version.py` writes the short commit hash into
`firmware/main/version.h`. The display uses it to decide whether an OTA
image is newer.

## 2. Build and flash

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

On Windows the port is something like `COM5`. Leave the monitor with
`Ctrl+]`.

Every push to GitHub also builds the firmware in Actions. The artifact
contains no Wi-Fi credentials, so it is useful together with the NVS step
below.

## 3. Wi-Fi and machine ID

There are two ways to give the display your Wi-Fi details.

### Option A: NVS (recommended)

Settings are stored in the `panel` NVS namespace and take priority over
anything compiled in. They survive firmware updates, and no password ever
ends up in a build.

Create `local/display-private.json` (git-ignored):

```json
{
  "ssid": "MyWifi",
  "password": "my-password",
  "machine_id": "my-mill"
}
```

Build the NVS image:

```sh
python -m pip install esp-idf-nvs-partition-gen==0.1.9 esptool
python tools/provision_nvs.py local/display-private.json --output local/display-nvs.bin
```

The result is a **complete** NVS partition (0x6000 bytes), not a patch: it
replaces whatever NVS holds. Back up the flash before writing it the first
time:

```sh
python -m esptool --chip esp32s3 --port PORT read-flash 0 ALL local/flash-backup.bin
python -m esptool --chip esp32s3 --port PORT write-flash 0x9000 local/display-nvs.bin
```

The JSON, the NVS image and the flash backup all contain your Wi-Fi
password. Keep them in `local/`.

### Option B: menuconfig

```sh
idf.py menuconfig
```

Under **LinuxCNC wireless display** set the SSID, password and machine ID.
These end up in `firmware/sdkconfig` and inside the binary, so don't share
that build. You can keep personal defaults in `firmware/sdkconfig.local`
(also git-ignored) and use:

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local" reconfigure
```

## 4. First boot

On the USB log you should see something like:

```text
I (1234) panel: discovery v2: machine=my-mill screen=a1b2c3d4e5f6
```

That `screen=` value is what goes into `screen_id` in `local/host.json` on
the LinuxCNC PC.

Status shown on the Classic page:

| Text | Meaning |
|---|---|
| NO WIFI | Not connected to the access point |
| NO DATA | Connected, but no valid stream from the PC |
| WAITING FOR HAL | The PC is sending, but `panel_wifi.enabled` is false |
| DEMO | The PC is running `--demo` |
| CONNECTED | Live data from HAL |

## Other menuconfig options

| Option | Default | |
|---|---|---|
| `PANEL_RPM_MAX` | 5000 | Gauge full scale. Display only, it doesn't limit anything. |
| `PANEL_RPM_WARNING` | 2500 | Where the red arc starts. |
| `PANEL_STALE_MS` | 1000 | Time without samples before NO DATA. |
| `PANEL_LCD_PCLK_MHZ` | 8 | Panel pixel clock. 8 MHz leaves PSRAM bandwidth for Wi-Fi; higher values made the image drift on my board. |
| `PANEL_BATTERY` | on | Battery icon. See [BATTERY.md](BATTERY.md). |
| `PANEL_HOST_PORT` / `PANEL_SCREEN_PORT` | 27180 / 27181 | UDP ports. Must match `host.json`. |
| `PANEL_UPDATE_PORT` | 27190 | OTA HTTP port on the PC. |

The Spindle page slider is fixed to 0-5000 RPM in `pages.c`. If your
spindle goes higher, change it there (and `PANEL_RPM_MAX`).

## Partition table

```text
nvs       0x9000   0x6000
otadata   0xf000   0x2000
phy_init  0x11000  0x1000
ota_0     0x20000  3 MB
ota_1     0x320000 3 MB
```

Two app slots with rollback. A new image boots once as "pending verify" and
is only kept after it receives a valid stream from the PC; otherwise the
bootloader goes back to the previous one. Details in [OTA.md](OTA.md).
