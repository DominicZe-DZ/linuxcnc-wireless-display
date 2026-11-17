# 01 Requirements

## Purpose

A small screen mounted on the machine that shows what the spindle and the
axes are doing, readable from where the operator stands, without a cable to
the control cabinet and without changing how the machine is controlled.

## Starting point (constraints)

| ID | Constraint |
|---|---|
| C-1 | The machine runs LinuxCNC (2.9 or newer, uspace) with a Mesa Ethernet card (7i76E) on a dedicated Ethernet port. |
| C-2 | The LinuxCNC PC has Wi-Fi on the home network. |
| C-3 | The display is a Waveshare ESP32-S3-Touch-LCD-2.8C: round 480x480 RGB panel, capacitive touch, 16 MB flash, 8 MB PSRAM. |
| C-4 | The realtime part of LinuxCNC (servo and base threads, Mesa link) must not be touched. |
| C-5 | Nothing may need the Internet, a cloud account or a server other than the LinuxCNC PC. |
| C-6 | The PC side may only use what a stock LinuxCNC install has (its Python 3, standard library). |

## Functional requirements

| ID | Requirement |
|---|---|
| FR-1 | Show spindle speed as an analog gauge and a number, 0 to 5000 RPM, with a warning zone from 2500 RPM. |
| FR-2 | Show current feed in mm/min. |
| FR-3 | Show X, Y, Z machine position. |
| FR-4 | Show spindle on/off and coolant (flood or mist) on/off. |
| FR-5 | Show the connection state: no Wi-Fi, no data, waiting for HAL, demo, connected. |
| FR-6 | Show work coordinates of the active WCS with three decimals, and machine coordinates on request (DRO page). |
| FR-7 | Optionally set a work zero for X, Y, Z or all three in the active WCS, with confirmation. |
| FR-8 | Optionally set spindle speed, start and stop the spindle. |
| FR-9 | Pages are changed by tapping the logo; no other part of the screen navigates. |
| FR-10 | Show an estimate of the battery charge when a LiPo cell is fitted. |
| FR-11 | The firmware can be updated over Wi-Fi from the LinuxCNC PC. |
| FR-12 | A demo mode sends fake data so the display can be tested without HAL. |
| FR-13 | Both ends find each other without fixed IP addresses (DHCP on both). |
| FR-14 | Recover automatically from Wi-Fi loss, PC restart, display restart or a new DHCP lease, without restarting LinuxCNC or the display. |

## Non-functional requirements

| ID | Requirement |
|---|---|
| NFR-1 | Values refresh at least 20 times per second when they change; the needle must move smoothly. |
| NFR-2 | A data loss is visible on screen within about 1 second (no frozen old values shown as live). |
| NFR-3 | Unchanged data must cause no redraw; a needle move must not redraw the whole screen (PSRAM bandwidth is shared with Wi-Fi). |
| NFR-4 | The PC component must stay lightweight: bounded work per cycle, no process spawned per sample, no disk write per sample. |
| NFR-5 | A missing value is never shown as zero. |
| NFR-6 | Wi-Fi credentials and keys never end up in the Git repository or in a published firmware image. |
| NFR-7 | The firmware never needs a cable again after the first flash (OTA with rollback). |
| NFR-8 | The code is testable without the machine: parsers, signatures, battery model and screen logic run on a PC. |

## Safety requirements

These come first. If any other requirement conflicts with them, they win.

| ID | Requirement |
|---|---|
| SR-1 | In its default configuration the system is read-only: it cannot change anything on the machine. |
| SR-2 | Control features only exist when the same secret key is configured on both sides. |
| SR-3 | No traffic for the display ever goes through the Mesa Ethernet link, and the PC refuses to run if the Wi-Fi and Mesa subnets overlap. |
| SR-4 | Commands are limited to a fixed list (speed, start, stop, zero) turned into fixed MDI blocks on the PC. No free G-code, no jog, no homing, no program start, no E-stop reset. |
| SR-5 | A command only runs in idle manual mode, re-checked on the PC right before execution. Never during a program or a pause. Stop is also allowed in degraded idle states. |
| SR-6 | Every command and answer is authenticated; replayed, expired, out-of-session or tampered messages are rejected. |
| SR-7 | A command whose result is unknown is never retried automatically. |
| SR-8 | No component of this project runs in a realtime thread or writes a HAL pin. |
| SR-9 | A firmware update only starts in idle manual mode, only installs an image signed for the current session, and rolls back if the new image doesn't work. |

## Out of scope

- Being a safety device. The hard-wired E-stop is the safety device.
- Logging data or guaranteeing every sample arrives.
- Working across routed networks, VLANs or the Internet.
- More than one display per PC (one machine/screen pair per sender instance).
