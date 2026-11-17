# Troubleshooting

| What you see | What to check |
|---|---|
| NO WIFI | 2.4 GHz SSID, password, signal, power. Check the USB log at boot. |
| NO DATA, Wi-Fi OK | `machine_id` and `screen_id` match on both sides, ports, router client isolation / guest network, sender actually running. |
| WAITING FOR HAL | `setp panel_wifi.enabled true` missing, or it runs before the `net` lines. |
| DEMO | The PC is running `--demo`, that's not machine data. |
| UPDATE THE HOST SOFTWARE (Spindle page) | The sender is an old version without the `ui` object. |
| CONTROL NOT PAIRED | No `control_key` in the display NVS. Expected if you didn't enable control. |
| `No module named hal` | Run it with LinuxCNC's Python (`/usr/bin/python3`), not a venv without system packages. |
| `the configured interface is not a Wi-Fi interface` | Wrong `interface` in `host.json`. Don't put the Mesa Ethernet there. |
| `Wi-Fi and Mesa subnets overlap` | Fix the addressing. Don't widen `mesa_network` just to make it start. |
| `pin ... already linked` in HAL | That pin is already on a signal. Add `panel_wifi.*` to the existing signal. |
| Classic page positions differ from the GUI DRO | The Classic page shows machine coordinates. The DRO page has WORK/MACHINE buttons. |
| Image drifting or flickering | Make sure it's the 2.8C board and that `PANEL_LCD_PCLK_MHZ` is 8. |
| Reboots with `stack overflow in task main` | Keep `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384` from `sdkconfig.defaults`. |

## Looking at the traffic

Not needed for normal use, but handy:

```sh
ip -br -4 addr
ip route get DISPLAY_IP from PC_WIFI_IP
halcmd show comp panel_wifi
halcmd show pin 'panel_wifi.*'
sudo tcpdump -ni wlan0 udp port 27180 or udp port 27181
```

The same `tcpdump` on the Mesa interface should show nothing on those
ports.

## Logs

- PC: `local/runtime.log` (start-up, link changes, NML errors, control
  results). It doesn't log every packet.
- Display: USB serial at 115200, e.g. `idf.py -p PORT monitor`.

When asking for help, include the LinuxCNC version, the commit you are on,
the exact error and your interface names. Remove your Wi-Fi password and
pairing key from anything you paste.
