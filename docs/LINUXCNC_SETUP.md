# LinuxCNC setup

This is the PC side: the `panel_wifi` HAL component that reads a few signals
and sends them to the display. It only needs the Python that comes with
LinuxCNC; there is nothing to compile and no extra packages.

Flash the display first ([FIRMWARE.md](FIRMWARE.md)), you need its Wi-Fi MAC
address for step 3.

## 1. Check the network

On the LinuxCNC PC:

```sh
ip -br -4 addr
ip route
```

Write down two things:

- The name of the **Wi-Fi** interface (`wlan0`, `wlp2s0`...) and its home IP.
- The subnet of the **Mesa** Ethernet link. On my F1 the 7i76E is at
  `10.10.10.10`, so the Mesa network is `10.10.10.0/24`.

The two must be different subnets. Don't change anything on the Mesa side to
install this, it isn't needed.

The display uses 2.4 GHz Wi-Fi. Both ends can use DHCP, but they have to be
on the same Wi-Fi subnet and the router must let clients talk to each other
and pass local broadcasts. Guest networks with "client isolation" won't work.
No Internet access is needed.

## 2. Copy the repository to the machine

As the same user that runs LinuxCNC (not root):

```sh
cd ~
git clone https://github.com/DominicZe-DZ/linuxcnc-wireless-display.git
cd linuxcnc-wireless-display
mkdir -p local
cp config/host_generic.example.json local/host.json
```

`local/` is ignored by Git. Your settings, the log file and published
firmware images live there.

Quick check that LinuxCNC's Python can see the HAL module:

```sh
/usr/bin/python3 -c 'import hal; print("hal OK")'
```

## 3. Edit `local/host.json`

```json
{
  "interface": "wlan0",
  "machine_id": "my-mill",
  "screen_id": "001122334455",
  "host_port": 27180,
  "screen_port": 27181,
  "rate_hz": 25,
  "mesa_network": "10.10.10.0/24",
  "rpm_input_scale": 1
}
```

| Key | Meaning |
|---|---|
| `interface` | Wi-Fi interface name from step 1. It must really be Wi-Fi, the sender checks. |
| `machine_id` | Any short name (a-z, 0-9, `-`, `_`). Must match the one stored on the display. |
| `screen_id` | Wi-Fi MAC of the display, 12 lower case hex digits, no colons. The display prints it on the USB log at boot (`discovery v2: machine=... screen=...`). |
| `mesa_network` | Mesa subnet. The sender refuses to run if the Wi-Fi subnet overlaps it. |
| `rate_hz` | Snapshots per second, 1 to 100. 25 is plenty. |
| `rpm_input_scale` | Multiplier for the RPM input. Use `60` if your signal is in rev/s (stepgen velocity), `1` if it is already RPM. |
| `control_key` | Optional, leave it out. See [CONTROL_CHANNEL.md](CONTROL_CHANNEL.md). |
| `firmware_port` | OTA server port, default 27190. `0` turns it off. Only used with a control key. |
| `data_dir` | Folder for `runtime.log` and OTA images. Default is `local/` in the repo. |

## 4. Test with fake data

Before touching any machine file:

```sh
/usr/bin/python3 run_panel.py --demo --config local/host.json
```

The display should switch to **DEMO** and show moving RPM and positions.
Stop it with Ctrl+C: within about a second the display shows **NO DATA**.
Start it again and it reconnects on its own, no need to reboot the display.

The demo doesn't load HAL and can run with LinuxCNC closed.

## 5. Hook it into HAL

Make a backup of your config folder first.

Copy one of the examples into your LinuxCNC config folder as
`panel_wifi.hal`:

- `config/panel_wifi_generic.hal.example`: a starting point for any machine.
- `config/panel_wifi_f1_probe_basic.hal.example`: what I run on my EMCO F1
  (Probe Basic, Mesa 7i76E, step/dir servo spindle).

Fix the paths on the `loadusr` line and the signal names. To see the
signals you have:

```sh
halcmd show sig
```

A few things to keep in mind:

- `panel_wifi` only **reads**. Adding it to an existing signal adds a reader,
  it doesn't change the value or add a writer.
- A pin can only belong to one signal. If, for example,
  `motion.current-vel` is already on a signal, add `panel_wifi.feed` to that
  signal instead of creating a new one.
- `feed` is expected in machine units per second (mm/s). The display shows
  mm/min.
- The positions on the Classic page are machine coordinates. The DRO page
  gets work coordinates from LinuxCNC directly, so you don't need to wire
  those.
- `setp panel_wifi.enabled true` goes last. Until then the display shows
  **WAITING FOR HAL**.

Then add the file to the `[HAL]` section of your INI, **after** the main HAL
file that creates the signals:

```ini
[HAL]
HALFILE = your_machine.hal
HALFILE = panel_wifi.hal
```

`loadusr -Wn` only waits for the pins to be created, never for the network.
If the Wi-Fi isn't up yet when LinuxCNC starts, the sender keeps retrying
every 2 seconds in the background.

To remove it, delete the `HALFILE = panel_wifi.hal` line and restart
LinuxCNC. Nothing else was changed.

## 6. Check it

Start LinuxCNC and confirm the component is there:

```sh
halcmd show comp panel_wifi
halcmd show pin 'panel_wifi.*'
```

Compare RPM, feed and XYZ with what your GUI shows. Jog, run the spindle,
switch coolant on and off.

Things worth testing once:

- Turn the display off and on, and drop the Wi-Fi for a moment. Both should
  recover on their own.
- Close LinuxCNC. The component receives SIGTERM and exits cleanly.
- If you are picky about latency, compare the servo thread max time with and
  without the component, under your normal load. It runs in userspace and
  does very little, but it still shares the CPU.

The sender writes to `local/runtime.log`. It doesn't log every packet, only
start-up, connection changes and errors.

## Demo program

`gcode/display_demo_f1.ngc` is an air-cutting program that moves X, Y and Z
together, steps the spindle through 1000, 2500 and 5000 RPM and cycles flood
and mist, so you can see everything on the display moving. It runs around
the current G54 zero. Read [DEVELOPMENT.md](DEVELOPMENT.md#demo-program)
before running it on your machine.
