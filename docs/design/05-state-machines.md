# 05 State machines

The code doesn't always have an explicit `state` variable for these; often
the state is a combination of a few flags. This chapter names the states so
the code can be read against them.

## Display link (main.c)

State = (`wifi`, `sock >= 0`, `host_known`, `have_sample`).

```mermaid
stateDiagram-v2
  [*] --> NoWifi
  NoWifi --> Discovering: got IP, socket open
  Discovering --> Live: first valid sample for our nonce, (learn PC address)
  Live --> Live: newer sample (same session)
  Live --> Discovering: no valid sample for 1 s, (new nonce)
  Live --> NoWifi: Wi-Fi event / IP changed, (close socket, new nonce)
  Discovering --> NoWifi: Wi-Fi event / IP changed
```

| State | Screen | Hello |
|---|---|---|
| NoWifi | NO WIFI | none |
| Discovering | NO DATA | broadcast, 1 per second |
| Live | CONNECTED / DEMO / WAITING FOR HAL | unicast to the PC, 1 per second |

Every way out of Live creates a new nonce. That is what makes any packet of
an earlier session unacceptable, even if it arrives late.

## PC link (app.py)

State = (`transport`, `nonce`, time since `last_hello`).

```mermaid
stateDiagram-v2
  [*] --> NoLink
  NoLink --> Listening: WifiTransport created
  NoLink --> NoLink: error, retry in 2 s
  Listening --> Sending: valid hello for our pair
  Sending --> Sending: hello again (refresh, maybe new nonce/address)
  Sending --> Listening: 3 s without hello
  Listening --> NoLink: link changed / socket error
  Sending --> NoLink: link changed / socket error
```

In Listening the PC sends nothing but keeps reading, so a display that
appears an hour later is picked up straight away.

## Command, display side (pages.c + main.c)

```mermaid
stateDiagram-v2
  [*] --> Idle
  Idle --> Queued: button / slider and permitted()
  Queued --> Sent: main.c signs and sends (pending_id set)
  Queued --> Idle: main.c can't send -> "dropped" / "error"
  Sent --> Sent: ACK "pending"
  Sent --> Idle: final ACK (ok, busy, rejected...)
  Sent --> Idle: 2.5 s, link lost or stale -> "timeout"
```

- Only one command can be Queued or Sent. Further slider moves set
  `pending_resync`; Stop pressed meanwhile sets `stop_queued`. Both are
  sent as soon as the state goes back to Idle.
- "timeout" is final: Start and zero are never resent (SR-7). Only a speed
  is re-sent, because sending the same speed twice is harmless.

## Command, PC side

### Admission (`Control.receive`, main thread)

```text
datagram
  |- key configured?                         no  -> ignore
  |- v3 command, our machine/screen/session/nonce, id format,
  |  action/axis/rpm/wcs in range, MAC correct?   no  -> ignore (no answer)
  |- (nonce, id) seen before?                yes -> same ACK again, if same MAC
  |- worker busy?                            yes -> ACK "busy"
  |- token current and not expired?          no  -> ACK "expired"
  |- state 'manual' or 'mode'?               no  -> ACK <state>
  |- zero and WCS changed?                   yes -> ACK "wcs"
  '- admit: remember speed, snapshot spindle rotation,
            submit to worker, ACK "pending"
```

A bad signature gets no answer at all: an attacker learns nothing.

### Execution (`MdiExecutor.execute`, worker thread)

```mermaid
stateDiagram-v2
  [*] --> Check1
  Check1 --> Reject: state not manual, (Stop also accepts busy, home, css, override, mode)
  Check1 --> Stored: speed with spindle stopped
  Check1 --> Expired: deadline passed
  Check1 --> ToMDI: OK
  ToMDI --> Abort: mode switch not confirmed in 2 s
  ToMDI --> Check2
  Check2 --> Abort: state changed / WCS changed /, spindle stopped meanwhile
  Check2 --> RunBlock
  RunBlock --> Abort: block not confirmed in 2 s
  RunBlock --> Verify
  Verify --> BackToManual
  BackToManual --> Done
  Abort --> [*]: return to manual only if idle
  Done --> [*]: ok / unconfirmed / changed
  Reject --> [*]
  Stored --> [*]: "stopped"
  Expired --> [*]
```

"Verify" compares the effect with the request: new modal S, M3 or M5 as
expected, or the requested work coordinates now within 0.001 mm of zero.

## OTA download

Implemented in `update.c`.

```mermaid
stateDiagram-v2
  [*] --> IDLE
  IDLE --> CONNECT: panel_upd_start()
  CONNECT --> CONNECT: connecting (non-blocking)
  CONNECT --> REQUEST: TCP up
  REQUEST --> HEADERS: GET sent
  HEADERS --> DOWNLOAD: 200, Content-Length = signed size,, not chunked, not compressed
  DOWNLOAD --> DOWNLOAD: chunk written + hashed
  DOWNLOAD --> VERIFY: all bytes received
  VERIFY --> DONE: SHA-256 matches, esp_ota_end OK,, boot slot switched
  CONNECT --> FAIL: 2 s
  HEADERS --> FAIL: 5 s / bad header
  DOWNLOAD --> FAIL: 15 s idle / < 6 KB/s / too long / closed early
  VERIFY --> FAIL: hash or image check fails
  DONE --> [*]: main.c restarts
  FAIL --> IDLE: main.c retries after 10 min
```

After the restart:

```mermaid
stateDiagram-v2
  [*] --> PendingVerify: bootloader starts new slot
  PendingVerify --> Valid: first valid sample, (confirm_boot_once)
  PendingVerify --> RolledBack: reset before confirmation, (bootloader returns to old slot)
```

## Battery charging estimate

Implemented in `battery_model.c`.

```mermaid
stateDiagram-v2
  [*] --> NoReading
  NoReading --> NotCharging: 2.8 V <= V <= 4.5 V
  NotCharging --> Charging: step >= +35 mV / 10 s, or trend >= +12 mV / 300 s, or V >= 4.17 V (outside hold)
  Charging --> NotCharging: step <= -35 mV / 10 s, or (trend <= -10 mV / 300 s and V < 4.17 V), -> 2 min hold of the 4.17 V rule
  NotCharging --> NoReading: V out of range (history cleared)
  Charging --> NoReading: V out of range
```

On entering Charging, the 300 s history is overwritten with the current
voltage so the same jump isn't counted again.

## Page navigation (pages.c)

```mermaid
stateDiagram-v2
  Classic --> DRO: tap logo
  DRO --> Spindle: tap logo
  Spindle --> Classic: tap logo
  Classic --> Update: OTA starts (pages_ota)
  DRO --> Update: OTA starts
  Spindle --> Update: OTA starts
  Update --> Classic: OTA failed, pages_ota_end(), restores whichever page was open
```

On the Spindle page a light lock screen covers the controls while there is
no data, demo data, an old PC without `ui`, no pairing key, the machine is
off, or a program is running or paused.
