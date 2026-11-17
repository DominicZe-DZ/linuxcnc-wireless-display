# 07 Data formats

The message level description (fields and their meaning) is in
[../PROTOCOL.md](../PROTOCOL.md). This chapter is the byte and bit level.

## Identifiers

| Name | Made by | Bits | Text form | Lifetime |
|---|---|---|---|---|
| `nonce` | display, hardware RNG | 64 | 16 chars `[0-9a-f]` | until the link is lost |
| `session` | PC, `secrets.token_hex(8)` | 64 | 16 chars `[0-9a-f]` | one run of `panel_wifi` |
| `token` | PC | 64 | 16 chars `[0-9a-f]` | 1 s, or until nonce/state/WCS change |
| `id` | display, per command | 64 | 16 chars `[0-9a-f]` | one command |
| `screen` | display Wi-Fi MAC | 48 | 12 chars `[0-9a-f]` | permanent |
| `machine` | user | - | 1..32 chars `[a-z0-9_-]`, first `[a-z0-9]` | permanent |
| pairing key | user | 256 | 64 chars `[0-9a-f]` | until changed |

Only lower case hex is accepted, so each value has exactly one spelling and
can be compared as a plain string.

The display builds its 64-bit values from two calls to `esp_random()`
(32 bits each), printed as `%08lx%08lx`.

## Sequence number

`seq` is an unsigned 32-bit counter incremented by the PC for every sample,
wrapping from 4294967295 to 0. The display accepts a sample only if

```text
delta = (candidate - previous) mod 2^32
newer = delta != 0 and delta < 2^31
```

That is RFC 1982 style serial number arithmetic: correct across the wrap,
and it rejects duplicates (delta 0) and reordered packets (delta >= 2^31).

## Presence mask

`panel_sample_t.present`, one byte. Bit set = the channel holds a real value.

```text
 bit   7    6        5           4   3   2   1     0
     +---+-------+------------+---+---+---+------+-----+
     | - |coolant| spindle_on | z | y | x | feed | rpm |
     +---+-------+------------+---+---+---+------+-----+
```

Defined in `telemetry.h` as `PANEL_CH_*` (bit numbers) and `PANEL_BIT(n)`.
Examples:

```text
rpm + feed + spindle_on            = bits 0,1,5   = 1+2+32    = 35  = 0x23
rpm + feed + x + y + z             = bits 0..4    = 31             = 0x1F
all seven channels                 = bits 0..6    = 127            = 0x7F
```

A channel sent as `null` keeps its bit at 0; the screen shows dashes
(NFR-5).

## Signed strings

Signatures are HMAC-SHA256 over a canonical ASCII string, never over the
JSON text. The key is the 32 raw bytes encoded by the 64 hex characters
(`"a3..."` -> `0xa3, ...`). The result is written as 64 lower case hex
characters.

| Message | Canonical string |
|---|---|
| command | `3\|command\|<machine>\|<screen>\|<session>\|<nonce>\|<id>\|<token>\|<action>\|<rpm>\|<axis>\|<wcs>` |
| ack | `3\|ack\|<machine>\|<screen>\|<session>\|<nonce>\|<id>\|<result>` |
| firmware offer | `fw\|<version>\|<size>\|<sha256>\|<nonce>` |

`rpm`, `wcs` and `size` are decimal integers without sign or leading
zeros. None of the fields can contain `|` (all have restricted character
sets), so the concatenation can't be ambiguous.

Example command string:

```text
3|command|my-mill|a1b2c3d4e5f6|fedcba9876543210|0123456789abcdef|1111222233334444|2222222222222222|start|1500|-|2
```

MAC comparison is constant time on both sides (`hmac.compare_digest` in
Python, XOR/OR loop in C).

## Datagram limits

- At most 1200 bytes: under a 1500-byte Ethernet MTU minus IP/UDP headers
  and Wi-Fi encapsulation, so no IP fragmentation.
- ASCII JSON, no NUL bytes, nothing after the document except whitespace.
- Numbers: finite, magnitude <= 1e7. Integers (seq, wcs, selected, rpm,
  size) must be whole numbers.

A typical sample with `ui` is about 350 to 450 bytes.

## `in_pktinfo` (Linux, PC side)

Ancillary data attached with `IP_PKTINFO` (option 8) on the UDP socket:

```text
 offset  0                4                8               12
         +----------------+----------------+----------------+
         | ipi_ifindex    | ipi_spec_dst   | ipi_addr       |
         | uint32, host   | IPv4, network  | IPv4, network  |
         | byte order     | byte order     | byte order     |
         +----------------+----------------+----------------+
```

Python: `struct.pack("=I4s4s", index, inet_aton(source), b"\0"*4)`. On
receive only `ipi_ifindex` is used, to drop anything that didn't come in on
the Wi-Fi card.

## NVS (display)

Namespace `panel`, all values strings:

| Key | Content | Required |
|---|---|---|
| `ssid` | Wi-Fi SSID, 1..31 bytes | yes (or menuconfig) |
| `password` | Wi-Fi password, 0..63 bytes | yes (or menuconfig) |
| `machine_id` | must match `host.json` | default `emco-f1` |
| `control_key` | 64 hex chars | only for control/OTA |

`tools/provision_nvs.py` writes a CSV and calls Espressif's
`nvs_partition_gen` to produce a full 0x6000-byte partition image for
address 0x9000.

## Partition table

```text
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
otadata,  data, ota,     0xf000,   0x2000
phy_init, data, phy,     0x11000,  0x1000
ota_0,    app,  ota_0,   0x20000,  0x300000
ota_1,    app,  ota_1,   0x320000, 0x300000
```

App slots must start on a 64 KiB boundary (0x20000 and 0x320000 do).

## Image assets

All generated by scripts in `tools/` and committed as C arrays.

### ALPHA_8BIT masks

Logos, icons, the battery bolt. One byte per pixel = coverage (0
transparent, 255 solid). Row by row, top to bottom, left to right, no
padding. Size = width x height bytes. The colour comes from LVGL's
`img_recolor` at draw time.

Example: the DominicZe logo on the DRO header is 190 x 40 = 7600 bytes.

### TRUE_COLOR_ALPHA (needle)

Three bytes per pixel for LVGL 8 at 16-bit colour depth:

```text
 byte 0            byte 1            byte 2
 +----------------+----------------+--------+
 | RGB565 low     | RGB565 high    | alpha  |
 +----------------+----------------+--------+

 RGB565 (16 bits):  R R R R R G G G G G G B B B B B
                    15      11 10       5 4       0

 pixel = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
 byte0 = pixel & 0xFF, byte1 = pixel >> 8          (little endian)
```

8-bit red keeps its top 5 bits, green 6 (the eye is most sensitive to
green), blue 5. Needle: 256 x 40 px x 3 = 30720 bytes.

### Fonts

LVGL font format produced by `lv_font_conv` with 4 bits per pixel
(16 coverage levels), uncompressed, only the glyphs needed (e.g. digits,
`.` and `-` for the big numbers). Each file starts with the exact command
that generated it.

## host.json

```json
{
  "interface": "wlp2s0",          // required, Linux interface name, <= 15 chars
  "screen_id": "a1b2c3d4e5f6",    // required, display Wi-Fi MAC
  "machine_id": "emco-f1",
  "host_port": 27180,
  "screen_port": 27181,
  "rate_hz": 25,                  // 1..100
  "mesa_network": "10.10.10.0/24",
  "rpm_input_scale": 1.0,         // 0 < x <= 1000
  "control_key": "",              // "" or 64 hex
  "firmware_port": 27190,         // 0 disables OTA
  "data_dir": ""                  // "" = <repo>/local
}
```

(Comments shown for explanation only; JSON itself doesn't allow them.)

## display.json (OTA record)

```json
{"version": "1a2b3c4", "bytes": 1257536, "sha256": "<64 hex>"}
```

`version` is `[0-9a-z]{1,15}` and must be the build ID compiled into the
image. The size used is always the real size of `display.bin`.
