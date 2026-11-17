# Optional control channel

> **Read this whole page before enabling it.** The display can start and
> stop the spindle and set work zeros on a real machine. It is off by
> default, and it is not a safety device. Your hard-wired E-stop is what
> keeps you safe, not this.

Without a pairing key nothing on this page exists: the sender never creates
a `linuxcnc.command()`, and the display keeps the zero buttons and the
Spindle page locked.

## What it can do

Only four actions, all turned into fixed MDI blocks on the PC:

| Action | MDI block sent |
|---|---|
| Spindle speed | `S<rpm>` (only if the spindle is already turning) |
| Start | `S<rpm> M3` |
| Stop | `M5 S0` |
| Zero X, Y, Z or XYZ | `G10 L20 P<wcs> X0 Y0 Z0` (only the chosen axes) |

There is no jog, no homing, no program start, no E-stop reset, no free
G-code, no shell and no file access from the display. The values that go
into those blocks come from bounded fields (RPM 0-5000, WCS 1-9, axis
x/y/z/xyz), never from text received over the network.

The 5000 RPM ceiling is hard coded on both sides because that's my
spindle. If yours is different, change it in `control.py`, `cnc_status.py`
and `pages.c`.

## When a command is accepted

The PC checks the real LinuxCNC state, whatever the display shows. A command
only runs in **idle manual mode**:

- machine on, no E-stop, not paused, no program in AUTO;
- interpreter idle, queues empty, in position, not moving;
- all joints homed;
- spindle override at 100 % and no G96 (CSS).

Otherwise the display shows why (PROGRAM RUNNING, AXES NOT HOMED,
OVERRIDE NOT 100 %...). The one exception is **Stop**: it is also accepted
in degraded but harmless states (not homed, override, CSS, busy, MDI), never
while a program runs or is paused.

The sequence on the PC for every command:

1. Check the state, switch to MDI, wait.
2. Check the state **again**. If anything changed (someone pressed cycle
   start, the WCS changed, the spindle stopped), give up.
3. Send the block, wait, check the result (new zero, modal S, M3/M5).
4. Switch back to manual, only if the interpreter is idle.

If a timeout or someone else's activity gets in the way, it never forces
manual mode and never aborts a program. Commands run one at a time in a
single worker thread, so the 25 Hz telemetry keeps flowing.

Probe Basic doesn't really use an explicit manual mode, so if the task is
left sitting in an idle MDI after one of our commands, the sender switches
it back to manual by itself after a few seconds (never with a program,
pause or motion going on).

## What "OK" means

The display shows CONFIRMED BY LINUXCNC when the PC saw the expected result
in LinuxCNC's status (new modal S, M3/M5, zero applied). It does **not**
mean the spindle has physically reached the speed.

If the answer is lost or takes more than 2.5 s, the display shows
NOT CONFIRMED and does not resend Start or a zero. The command may still
have run; look at the machine.

## Pairing key

32 random bytes written as 64 lower case hex characters. The same key goes
on both sides.

Generate one:

```sh
python3 -c "import secrets; print(secrets.token_hex(32))"
```

**PC**, in `local/host.json`:

```json
"control_key": "<64 hex characters>"
```

**Display**, in the NVS JSON used by `tools/provision_nvs.py` (together with
the Wi-Fi settings, the NVS image is always complete):

```json
{
  "ssid": "MyWifi",
  "password": "my-password",
  "machine_id": "my-mill",
  "control_key": "<the same 64 hex characters>"
}
```

Flash it as described in [FIRMWARE.md](FIRMWARE.md#option-a-nvs-recommended).
The key is never compiled into the firmware, never logged and never sent
over the network. Anyone who has it can send commands to your machine, so
keep it out of Git, screenshots and forum posts.

## Protocol details

Commands and answers travel on the normal telemetry sockets as `"v":3`.

Command fields, signed in this exact order joined with `|`:

```text
v|type|machine|screen|session|nonce|id|token|action|rpm|axis|wcs
```

Answer (ACK) fields:

```text
v|type|machine|screen|session|nonce|id|result
```

`mac` is HMAC-SHA256 over that string with the pairing key, as 64 lower
case hex characters. The key authenticates, it doesn't encrypt.

- `token` comes from the latest snapshot and is valid for one second. It
  changes whenever the nonce, the state or the WCS changes, and after every
  command.
- `id` is a random 64-bit value. A repeated request gets the same ACK back
  without running again; the last 128 are remembered.
- Only one command can be in flight. A second one gets `busy`.
- A bad signature is dropped silently. An expired token, wrong context or
  wrong state gets a signed rejection.

Results: `ok`, `pending`, `busy`, `expired`, `stopped` (speed stored while
the spindle is off), `program`, `paused`, `off`, `mode`, `home`, `css`,
`override`, `wcs`, `timeout`, `changed`, `unconfirmed`, `error`.

## Before you trust it

Test with no tool in the spindle and nothing on the table:

1. Without a key: confirm the display stays read-only.
2. With the key on both sides, in manual mode: change speed, start, stop,
   zero one axis. Check each result in your GUI.
3. Start a program and confirm everything is locked on the display.
4. Pause it and confirm the same.
5. Press your real E-stop with the spindle running from the display.

It works on my machine. Yours has a different HAL, a different spindle
drive and maybe a different GUI, so take nothing for granted.
