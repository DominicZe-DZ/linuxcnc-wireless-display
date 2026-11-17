# UDP protocol

One machine, one display, IPv4 on the same Wi-Fi network.

| | Port |
|---|---|
| LinuxCNC PC (`panel_wifi`) | UDP 27180 |
| Display | UDP 27181 |
| OTA firmware server on the PC | TCP 27190 (HTTP) |

All messages are ASCII JSON, at most 1200 bytes so they never fragment.
Unknown fields are ignored. Every telemetry packet is a full snapshot;
nothing is ever rebuilt from deltas and lost packets are not resent.

## Discovery (v2)

The display gets its address by DHCP and works out the broadcast address of
its subnet. While it has no live stream it sends a hello there once per
second:

```json
{"v":2,"type":"hello","machine":"my-mill","screen":"001122334455","nonce":"0123456789abcdef"}
```

- `machine`: 1-32 characters, lower case letters, digits, `-` or `_`.
- `screen`: Wi-Fi MAC of the display, 12 lower case hex digits.
- `nonce`: 16 lower case hex digits, new for every session.

The PC accepts the hello only if it arrived on the Wi-Fi interface, from an
address in that subnet, from port 27181, with the configured machine/screen
pair. Truncated datagrams are dropped. Then it learns the display address.

The display learns the PC address from the first valid snapshot for its
nonce. From then on it only accepts that source and sends its hellos as
unicast. When the stream goes stale it goes back to broadcast with a new
nonce.

## Snapshot (v2)

```json
{"v":2,"type":"sample","machine":"my-mill","screen":"001122334455",
 "session":"fedcba9876543210","nonce":"0123456789abcdef","seq":42,
 "valid":true,"demo":false,
 "data":{"rpm":2450.0,"feed":10.0,"x":125.0,"y":-75.0,"z":-120.0,
         "spindle_on":true,"coolant":false},
 "ui":{...}}
```

| Field | Meaning |
|---|---|
| `session` | Random per sender run, 16 hex |
| `nonce` | Echo of the display's current nonce |
| `seq` | uint32, wraps around |
| `valid` | `panel_wifi.enabled`. False shows WAITING FOR HAL |
| `demo` | True forces the DEMO label |
| `rpm` | Spindle RPM, already multiplied by `rpm_input_scale` |
| `feed` | mm/s (the display multiplies by 60) |
| `x`, `y`, `z` | Machine position in mm |
| `spindle_on` | Spindle on, or really turning (see below) |
| `coolant` | Flood OR mist, combined on the PC |

A missing channel is `null`, never a fake zero. The C parser rejects wrong
types, non-finite numbers, magnitudes above 1e7, fractional sequence
numbers, truncated packets and trailing garbage after the JSON. Parsing is
all-or-nothing: on any error the previous sample stays on screen. An
invalid, duplicated, older or foreign packet never refreshes the "alive"
timer.

`spindle_on` is true if the HAL signal says so **or** the RPM feedback is
above 0.05 rev/s. On a step/dir spindle the "on" signal can be off while the
stepgen is still outputting pulses.

### The `ui` object

Optional. A sender without it still works with the Classic page.

```json
{"state":"manual","wcs":2,"work":[25.0,-25.0,-20.0],"selected":1500,
 "allowed":false,"token":"2222222222222222","spindle_running":true,
 "upd":{"version":"1a2b3c4","size":1258432,"sha256":"...","mac":"..."}}
```

| Field | Meaning |
|---|---|
| `state` | `manual` (idle manual), `mode`, `busy`, `home`, `css`, `override`, `program`, `paused`, `off`, `remote`, `unknown` |
| `wcs` | Active work system, 1 = G54 ... 9 = G59.3 |
| `work` | Work coordinates in mm, or `null` if not reliable |
| `selected` | Spindle speed to show on the Spindle page |
| `allowed` | Control commands accepted right now |
| `token` | Short-lived permission token for the next command |
| `upd` | Firmware offer, see [OTA.md](OTA.md) |

Work coordinates take G5x, tool offset, XY rotation, G92 and machine units
into account, and are always sent in mm.

## Recovery

- **Display**: 1000 ms without a valid sample, an IP change or a Wi-Fi
  reconnect drops the source and nonce and starts discovery again.
- **PC**: 3 seconds without a hello stops sending but keeps listening
  forever, so a display switched on much later still connects.
- **PC**: the Wi-Fi interface and address are checked every 2 seconds and
  the link is rebuilt if anything changed, without restarting LinuxCNC.

## Control (v3)

Commands from the display and their answers use `"v":3` with
`"type":"command"` and `"type":"ack"` on the same sockets. They are only
accepted when a pairing key is configured on both sides, and every message
carries an HMAC-SHA256. Full description in
[CONTROL_CHANNEL.md](CONTROL_CHANNEL.md).

## Limits

- Discovery needs local broadcast and client-to-client traffic on the
  router. It doesn't cross VLANs or subnets, and doesn't use mDNS.
- Telemetry is not signed. IDs and nonces prevent mix-ups, they are not
  authentication.
- Delivery of every sample is not guaranteed. This is a display, not a data
  logger and not a motion control link.
- Changing the meaning or unit of an existing field means a new protocol
  version: update the firmware, the sender, the tests and this file
  together.
