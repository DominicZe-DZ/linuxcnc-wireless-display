# Design documentation

This folder is the engineering description of the project: what it has to
do, how it is split up, why it is built this way and how it was checked. It
is written so that someone (including me, a few years from now) can pick the
project up cold and understand every part before touching the code.

The user guides (installation, firmware, troubleshooting) are one level up
in `docs/`. This folder doesn't repeat them; it explains the inside.

| Chapter | Contents |
|---|---|
| [01 Requirements](01-requirements.md) | What the system must do and must never do, with IDs used everywhere else |
| [02 System architecture](02-system.md) | Context, hardware blocks, networks, software blocks, data flow |
| [03 Host software](03-host.md) | `panel_wifi` on the LinuxCNC PC: modules, functions, threads, timing |
| [04 Firmware](04-firmware.md) | ESP32-S3 application: main loop, modules, functions, memory, display pipeline |
| [05 State machines](05-state-machines.md) | Link, command, OTA, battery and page navigation |
| [06 Sequences](06-sequences.md) | Message exchanges over time: discovery, loss, commands, OTA |
| [07 Data formats](07-data-formats.md) | Bit and byte level: presence mask, IDs, signatures, NVS, partitions, image assets |
| [08 Design decisions](08-decisions.md) | Each important choice, the alternatives and why they were rejected |
| [09 Verification](09-verification.md) | Requirement to test traceability, automatic and manual tests |

## Reading order

New to the project: 01, 02, then 08 to understand the reasoning. After that
03 or 04 depending on which side you are changing, and 05 to 07 as
reference while reading the code.

## Conventions

- Requirement IDs: `FR-n` functional, `NFR-n` non-functional, `SR-n`
  safety. Decisions: `D-n`.
- Diagrams are Mermaid (GitHub renders them) or plain text, so they can be
  edited with any text editor and reviewed in a diff.
- Sizes: KiB = 1024 bytes, MB in the partition table = MiB.
- "PC" means the LinuxCNC computer, "display" the ESP32-S3 board.

## Glossary

| Term | Meaning |
|---|---|
| HAL | LinuxCNC Hardware Abstraction Layer: pins and signals that connect components |
| userspace component | HAL component running as a normal Linux process (`loadusr`), outside the realtime threads |
| NML | LinuxCNC's internal messaging; `linuxcnc.stat()` reads the task status through it |
| MDI | Manual Data Input: LinuxCNC mode that runs single G-code blocks |
| WCS | Work coordinate system, G54..G59.3 (LinuxCNC index 1..9) |
| LVGL | Light and Versatile Graphics Library, version 8 here |
| nonce | Random value used once; here a 64-bit session identifier made by the display |
| HMAC | Keyed hash (here HMAC-SHA256) proving a message was made by someone holding the key |
| OTA | Over-the-air firmware update |
| NVS | ESP-IDF non-volatile storage, a key/value store in a flash partition |
