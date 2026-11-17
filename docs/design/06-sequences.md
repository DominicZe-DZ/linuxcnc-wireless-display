# 06 Sequences

Time goes down. `D` = display, `P` = PC (`panel_wifi`), `L` = LinuxCNC.

## Discovery and telemetry

```mermaid
sequenceDiagram
  participant D as Display
  participant P as panel_wifi
  participant L as LinuxCNC
  Note over D: gets IP by DHCP<br/>nonce N1 = random
  loop every 1 s until a sample arrives
    D->>P: hello {machine, screen, nonce N1} (broadcast)
  end
  Note over P: check interface, subnet, port, pair<br/>learn D address, nonce = N1
  loop every 40 ms while hellos keep coming
    P->>L: read HAL pins, NML snapshot
    P->>D: sample {session S, nonce N1, seq n, data, ui} (unicast)
  end
  Note over D: first valid sample: learn P address<br/>confirm boot image
  loop every 1 s
    D->>P: hello {nonce N1} (now unicast)
  end
```

## Losing and regaining the link

```mermaid
sequenceDiagram
  participant D as Display
  participant P as panel_wifi
  P-xD: samples stop (Wi-Fi drop, PC restart...)
  Note over D: 1 s without a valid sample<br/>show NO DATA, nonce N2 = random<br/>forget P address
  D->>P: hello {nonce N2} (broadcast)
  Note over P: 3 s without hello: stop sending, keep listening
  Note over P: hello N2 accepted
  P->>D: sample {nonce N2, ...}
  Note over D: CONNECTED again
  P-->>D: late sample {nonce N1}
  Note over D: rejected: wrong nonce
```

If the PC process restarted, its new samples carry a new session S2. The
display accepts them only after its own data went stale and it made a new
nonce, so two senders can never be mixed into one picture.

## Spindle start (control enabled)

```mermaid
sequenceDiagram
  participant U as User
  participant D as Display
  participant P as panel_wifi (main)
  participant W as worker thread
  participant L as LinuxCNC
  P->>D: sample {ui: state manual, allowed true, token T}
  U->>D: tap Start (speed 1500)
  D->>P: command {id I, token T, action start, rpm 1500, mac}
  Note over P: verify MAC, id new,<br/>token T valid, state manual
  P->>D: ack {id I, result pending, mac}
  P->>W: execute
  W->>L: stat: still idle manual?
  W->>L: mode MDI, wait
  W->>L: stat: still OK?
  W->>L: mdi "S1500 M3", wait
  W->>L: stat: M3 on, S = 1500?
  W->>L: mode MANUAL, wait
  W-->>P: "ok"
  P->>D: ack {id I, result ok, mac}
  Note over D: CONFIRMED BY LINUXCNC
```

While the worker runs, telemetry keeps flowing every 40 ms; the samples
show `state: remote` and `allowed: false`, so no second command can be
sent.

## Zero with a WCS change in between

```mermaid
sequenceDiagram
  participant U as User
  participant D as Display
  participant P as panel_wifi
  U->>D: tap X >0 (screen shows G55)
  Note over D: dialog remembers wcs = 2 (G55)
  P->>D: sample {wcs 3}  (someone selected G56 in the GUI)
  Note over D: dialog closes by itself
  U->>D: (if it was still open) tap Set zero
  Note over D: wcs changed -> REJECTED: WORK OFFSET CHANGED<br/>nothing is sent
```

If the change happens after the command left the display, the PC rejects
it with `wcs` (admission or second check), so a zero is only ever set in
the system the user saw.

## Lost ACK

```mermaid
sequenceDiagram
  participant D as Display
  participant P as panel_wifi
  D->>P: command {id I, start}
  P--xD: ack pending (lost)
  P--xD: ack ok (lost)
  Note over D: 2.5 s -> NOT CONFIRMED - NOT RESENT
  Note over D,P: the spindle may be running:<br/>the screen shows the real state on the next sample
```

If the same command datagram arrives twice (network duplicate), the PC
answers with the stored ACK and does not run it again.

## Firmware update

```mermaid
sequenceDiagram
  participant D as Display
  participant P as panel_wifi
  participant H as HTTP thread
  Note over P: display.bin + display.json published
  P->>D: sample {ui.upd: version V, size, sha256,<br/>mac = HMAC(fw|V|size|sha|nonce)}
  Note over D: V != own build, state manual,<br/>MAC OK for current nonce
  D->>H: GET /firmware.bin
  H->>D: 200, Content-Length = size, body
  Note over D: write inactive slot + SHA-256<br/>hash OK -> switch boot slot
  Note over D: restart into V (pending verify)
  D->>P: hello (new nonce)
  P->>D: sample
  Note over D: confirm image V as valid
```

If V never gets a sample (it doesn't boot properly, or can't join the
Wi-Fi), the next reset rolls back to the previous image.
