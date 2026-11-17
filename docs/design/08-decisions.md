# 08 Design decisions

Each entry: the problem, what was chosen, what was rejected and why. If you
want to change one of these, read the "why" first; most of them come from
something that went wrong or from a safety requirement.

## D-1 Userspace HAL component, not realtime

**Chosen:** `panel_wifi` is a Python userspace component (`loadusr`) with
`HAL_IN` pins only.
**Rejected:** a realtime component (`.comp`, `loadrt`/`addf`) or a hook in
the GUI.
**Why:** networking has unbounded latency and has no business in the servo
thread (C-4, SR-8). A display needs ~25 Hz, not 1 kHz. Being a HAL
component (not a GUI plugin) means it works with any GUI and starts and
stops with LinuxCNC. Python ships with LinuxCNC (C-6).

## D-2 UDP datagrams, not TCP, MQTT or WebSocket

**Chosen:** plain UDP, one full snapshot per datagram.
**Rejected:** TCP stream, MQTT broker, HTTP/WebSocket.
**Why:** for live values the latest one is all that matters. TCP would
retransmit stale samples and stall the stream behind a lost packet (head
of line blocking), exactly when the Wi-Fi is bad. MQTT needs a broker
(C-5) and a client library. With UDP a lost sample is simply replaced by
the next one 40 ms later.

## D-3 JSON on the wire

**Chosen:** compact ASCII JSON, at most 1200 bytes.
**Rejected:** packed binary structs, Protocol Buffers, CBOR.
**Why:** it can be read with `tcpdump -A` while debugging, Python has it
built in and cJSON is already in ESP-IDF. At 25 Hz x ~400 bytes = 10 KB/s,
size doesn't matter. The cost (parsing time) is under 1 ms on the ESP32.
Binary would save bytes nobody needs and lose readability.

## D-4 Discovery by broadcast hello, pairing by IDs

**Chosen:** the display broadcasts a hello with machine ID, its MAC and a
nonce; the PC answers by unicast to whoever sent a valid hello.
**Rejected:** fixed IP addresses, DHCP reservations, mDNS.
**Why:** fixed addresses break when the router is changed or reset, and
asking a machine owner to set up DHCP reservations is asking too much
(FR-13). mDNS needs a responder on both sides and doesn't always work on
consumer routers. A broadcast on the local subnet is the simplest thing that
works everywhere clients can talk to each other. Pairing by machine ID +
screen MAC means two machines and two displays in the same workshop can't
mix up.

## D-5 Pin traffic to the Wi-Fi interface

**Chosen:** `IP_PKTINFO` on receive (filter by interface index) and on send
(force interface and source), plus a startup check that the subnets don't
overlap.
**Rejected:** trusting the routing table, binding the socket to the Wi-Fi
IP, `SO_BINDTODEVICE`.
**Why:** SR-3. The routing table can change (a cable plugged in, a default
route through Ethernet) and the Mesa link must never see this traffic.
Binding to an IP breaks when DHCP gives a new address, and it doesn't stop
broadcast reception from other interfaces. `SO_BINDTODEVICE` needs root
privileges on most systems; `IP_PKTINFO` doesn't.

## D-6 Full snapshots, presence bits, all-or-nothing parsing

**Chosen:** every packet carries every value; missing values are `null`
with a presence bit; the parser commits a sample only if every field is
valid.
**Rejected:** sending only changes, using 0 for "unknown", partial updates.
**Why:** with deltas, one lost packet leaves the screen wrong until the
value changes again. A zero for "unknown" is dangerous on a machine
(NFR-5): "Z 0.000" and "no idea where Z is" must look different. Partial
updates could show X from one moment and Y from another.

## D-7 Nonce per display session, session per PC run

**Chosen:** the display makes a new random nonce every time it loses the
stream; samples must echo it. The PC adds its own session ID, and the
display only accepts increasing sequence numbers within one session.
**Rejected:** relying on sequence numbers only, timestamps.
**Why:** after a PC restart the sequence starts at 0 again and would be
"older" than what the display has; after a display restart old packets
could still be in flight. The nonce makes every session distinct without
synchronised clocks. Timestamps would need NTP on both ends.

## D-8 HMAC-SHA256 with a pre-shared key for control

**Chosen:** 32-byte key typed into both sides; every command and every ACK
carries HMAC-SHA256 over a canonical string; short-lived tokens; ID
de-duplication.
**Rejected:** no authentication, TLS/DTLS, public keys.
**Why:** SR-2, SR-6. Anyone on the LAN can send UDP, so unauthenticated
commands are out of the question. DTLS needs certificates, a much bigger
code path and a handshake that hurts on a lossy link. The data isn't
secret (it's spindle speeds), it only needs to be genuine, which is
exactly what an HMAC gives. The token (1 s) plus nonce plus ID stop replay
of a captured command. A pre-shared key is easy to explain and to change.

## D-9 Fixed MDI blocks built on the PC, checked twice

**Chosen:** four actions, each turned into a fixed MDI block on the PC
(`S.. M3`, `M5 S0`, `S..`, `G10 L20 P.. X0..`), with the state checked
before switching to MDI and again right before the block.
**Rejected:** sending G-code from the display, using
`linuxcnc.command().spindle()` directly, writing HAL pins.
**Why:** SR-4, SR-5. G-code from the network would turn the display into a
remote MDI terminal. `command.spindle()` is accepted by LinuxCNC even
while a program runs in AUTO, which is exactly when the display must not
interfere. Writing HAL pins would bypass the interpreter and fight with
whatever else drives those pins. Going through MDI means LinuxCNC's own
rules apply (it refuses MDI during AUTO), and the second check closes the
window between "allowed" and "executed". The zero uses `G10 L20` so
LinuxCNC computes the offset itself, with the same rotation, tool and G92
handling as its own GUI; Python never subtracts offsets.

## D-10 Partial refresh, one framebuffer, 8 MHz

**Chosen:** LVGL renders only invalidated areas into a 20-line internal
buffer that is copied into one permanent PSRAM framebuffer; pixel clock
lowered to 8 MHz; values only written when their text changes.
**Rejected:** full-screen redraws, double buffering, the demo's default
pixel clock.
**Why:** this was learned on the hardware. With full redraws and double
buffering the image drifted sideways and flickered under Wi-Fi load, and
the main task overflowed its stack. The RGB panel is fed from PSRAM by DMA
continuously; every extra full-screen copy competes with that and with
the Wi-Fi buffers. Partial refresh makes an unchanged sample cost nothing
(NFR-3, verified by the native render test).

## D-11 One main loop on the display

**Chosen:** network, parsing, battery, OTA and LVGL all in one loop.
**Rejected:** separate FreeRTOS tasks with queues and mutexes.
**Why:** LVGL isn't thread-safe; with one loop there is nothing to lock and
no priority inversion to debug. Each step is bounded (8 datagrams, one
8 KiB chunk, one ADC burst), so the loop stays well under its 5 ms tick
budget except while LVGL renders.

## D-12 OTA from the PC over HTTP, signed per session, with rollback

**Chosen:** the PC serves the image; the offer travels inside telemetry,
signed with the pairing key and the current nonce; the display checks
size and SHA-256 while writing; two app slots with rollback.
**Rejected:** OTA from GitHub or another server, HTTPS, unsigned updates.
**Why:** C-5 (no Internet), NFR-7 (no cable). HTTPS would need a
certificate on the PC; the HMAC + SHA-256 already guarantee the image is
the one the key holder published, and a man in the middle can only make
the download fail. Rollback means a bad image can never leave the
display unusable. Updates only start in idle manual mode, never during a
job (SR-9).

## D-13 Battery charge estimated from voltage only

**Chosen:** filtered voltage, LiPo curve, charging guessed from voltage
steps and trends.
**Rejected:** fuel gauge chip, reading the charger status pin.
**Why:** the board has neither; STAT of the ETA6098 isn't wired to the
ESP32. The estimate is documented as an estimate (FR-10, BATTERY.md).

## D-14 Secrets in NVS, never in the build

**Chosen:** Wi-Fi credentials and pairing key in NVS, flashed from a local
JSON; menuconfig only as fallback.
**Rejected:** compiling them in.
**Why:** NFR-6. A firmware image built in CI can be shared and published,
and the same image works on any machine once its NVS is written. NVS also
survives OTA updates.

## D-15 Waveshare ESP32-S3-Touch-LCD-2.8C

**Chosen:** this board.
**Why:** round 480x480 IPS, touch, PSRAM for the framebuffer, Wi-Fi and a
LiPo charger on one board, all at a hobby price. The round shape suits a
gauge. Its drivers come from the official demo (fetched and checked by
hash rather than copied into the repo, because the demo has no stated
license).
