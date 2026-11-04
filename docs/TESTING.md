# Testing

Results of the test plan in [spec.md](spec.md) §8.

## 2026-10-30 — desk test, Raspberry Pi 3 Model B

Host: Raspberry Pi OS (Debian 13 trixie), kernel 6.18.50+rpt-rpi-v8, can-utils from the distro. No CAN bus attached.

| Test | Result |
|---|---|
| Enumeration | Pass. `lsusb`: `1d50:606f OpenMoko, Inc. Geschwister Schneider CAN adapter`; mainline `gs_usb` binds and creates `can0` with no host setup. `ip -details` reports the device's bit-timing limits (`tseg1 1..16 tseg2 1..8 sjw 1..4 brp 2..16384 brp_inc 2`) |
| Loopback, 31 frames (`loopback on`, 250 kbit/s) | Pass. TX 31, RX 62 (device loopback + host local echo), 0 errors. Past 10 frames without stalling, so TX echo releases the host's slots |
| Loopback burst, 2000 frames (`cangen -g 0`) | Pass. TX 2031, RX 4062, 0 errors, 0 dropped, 0 overruns |
| Normal mode, no bus (unacknowledged TX) | Pass. Error frames for warning then passive; state ERROR-PASSIVE, TX error counter 128, stays error-passive (ACK errors don't push past 128). `restart-ms` is refused ("Device doesn't support restart from Bus Off"), as expected: the firmware recovers from bus-off itself |
| Status LED | Pass (checked by eye): red with no host driver (Windows); blue with `can0` down; dim green when up; bright green flash per frame; orange when error-passive; back to blue on `ip link set can0 down`; blinking magenta for `ethtool -p can0` (identify). Bus-off (blinking red) not yet seen |

Bug found and fixed before this run: `pdMS_TO_TICKS(2)` was 0 at the default 100 Hz tick, so the USB IN task busy-looped at priority 7 and starved the main loop (status LED never updated). Now a 1 kHz tick and a one-tick minimum wait.

Not yet tested: bus-off and recovery (needs a real fault, e.g. CANH shorted to CANL), unplug/replug while up, two-adapter bus test, and a real NMEA 2000 bus (listen-only beside an existing gateway, then normal mode).

## 2026-10-30 — USB console and button-free reflash, same desk Pi

| Test | Result |
|---|---|
| Composite enumeration | Pass. `gs_usb` binds interface 0 (`can0`); `cdc_acm` binds interface 1 (`/dev/ttyACM0`, `/dev/serial/by-id/usb-n2k_usb_can_*-if01`). The Pi's `gs_usb` module aliases all end in `in00`, so it only ever claims interface 0 |
| Console commands | Pass. `stats` prints uptime, last reset reason, CAN state/counters; unknown input lists the commands |
| Logs on the console | Pass. `can_bus: started ... 250000 bit/s loopback` / `stopped` appear when `can0` goes up/down |
| Regression: 2000-frame loopback burst with the console open | Pass. TX 2000, RX 4000, 0 errors |
| 1200-baud touch → ROM bootloader | Pass after a fix: the first version crashed (USB stack torn down while stdout still pointed at the CDC port) and rebooted into the app. Detaching the console first fixed it; the device then re-enumerates as `303a:1001` |
| `tools/flash.sh` from the running firmware | Pass, twice (once starting in the bootloader, once from the running firmware): touch → bootloader → write + verify → back to the firmware with `can0`, no button |

Found on the way: after a flash the chip stayed in the bootloader. Two causes, both handled in `tools/flash.sh`: `RTC_CNTL_FORCE_DOWNLOAD_BOOT` survives resets and has to be cleared, and with `--before no_reset` esptool leaves DTR (the boot-mode override on USB-Serial/JTAG) as Linux set it, so the final reset either does nothing or lands back in the bootloader.

Host notes: Debian's `esptool` package (4.7.0) ships without the flasher stub, so it needs `--no-stub`. One enumeration after a reset logged `device descriptor read/all, error -32` and succeeded on the kernel's retry; not seen again.

## 2026-11-01 — live NMEA 2000 bus, boat "Nootka" (nootcore, Raspberry Pi)

First deployment on a real N2K bus, as the Signal K input, replacing an Actisense-format-over-serial Atom gateway (only one N2K drop available, so this adapter took the Actisense's spot; the old gateway was removed from the bus and unplugged).

| Test | Result |
|---|---|
| Enumeration | Pass. `lsusb` → `1d50:606f`; `can0` created DOWN with the expected bit-timing limits; CDC console at `/dev/serial/by-id/usb-n2k_usb_can_AtomS3_CAN_gateway__gs_usb__...-if01` → `ttyACM0`; `gs_usb` bound cleanly |
| Link up on live bus | Pass. `sudo ip link set up can0 type can bitrate 250000` → `state ERROR-ACTIVE`, berr-counter 0, 0 bus-errors, 0 bus-off. Error-active (not error-passive) on the populated bus confirms our node is ACKing real traffic — wiring good: CANH, CANL, and the drop's NET-S− (GND) wire landed on the base's GND pin (high-Z shield net, isolation intact). LED green with flashes |
| Traffic / decode | Pass. `candump` streamed classic 29-bit frames; every frame decoded to a known real source — SA 0 (Garmin GPS24xd: PGN 129025/129026/127250) and SA 115 (Raymarine E22158 SeaTalk→N2K bridge: 130306 wind, 129029 GNSS, 129540, 128275, 129033). **Zero phantom source addresses** in the sample. (VHF SA 1 and Fusion SA 12 simply weren't transmitting.) |
| Signal K | Pass. Server native (out of Docker). Added NMEA 2000 → **Canbus (canboatjs)** on `can0` as connection `pi_can0`, normal mode; disabled the old Actisense input. ~46.6 deltas/s |
| Boot persistence | Pass **after a fix** (see below). With a corrected `80-can.network`, `can0` comes up `ERROR-ACTIVE` at 250k on cold boot with no manual command; Signal K reconnects automatically |

**Bug in our own host-setup docs, found here:** the `80-can.network` snippet in the README and spec §7 included `RestartSec=100ms`. That maps to `restart-ms`, which this firmware refuses ("Device doesn't support restart from Bus Off" — already noted in the desk test above). systemd-networkd logged the config as failed, retried, and then *"The interface entered the failed state frequently, refusing to reconfigure it automatically"* — so after a reboot `can0` had the bitrate set but stayed **DOWN**. Removing the `RestartSec=` line fixed it; networkd's default `ActivationPolicy=up` then brings the link up. README and spec §7 corrected.

NetworkManager coexistence (was UNVERIFIED): `nmcli device status` shows `can0` as `unmanaged`, and systemd-networkd was `disabled/inactive` before this — enabling it only affects interfaces matching a `.network` file (`can0` only), so eth0/wlan0 stay with NetworkManager. No conflict.

## 2026-11-04 — USB wedge found + fixed; 16 h soak, boat "Nootka"

**The wedge.** On 2026-11-01, during a full-fleet test (every N2K device powered on, plus Signal K bridging AIS out onto `can0`), the gateway went dark after ~37 min under that load. dmesg on the Pi: `gs_usb … can0: failed to xmit URB … -EPIPE` (the bulk OUT endpoint stalled), then `usb 1-1.4.2: USB disconnect` — and it never re-enumerated. The CAN side stayed alive the whole time (LED green, still reading the bus); only the USB peripheral died, so `can0` vanished on the host until a physical replug. Not a CAN fault (bus was error-free), not the Pi's USB (other USB devices on the same hub kept working) — a device-side TinyUSB/DWC2 stall under sustained bulk traffic.

**Root cause.** The firmware routed all its logging (via `tinyusb_console_init`) into the CDC-ACM console endpoint — and nothing on the host ever opened that port. Under load, writes into an undrained USB endpoint congested the shared USB stack and stalled the vendor OUT endpoint.

**Fix.**
- **Removed the CDC console entirely** — single gs_usb interface, logs to UART0 (not exposed), never a USB endpoint. Cost: no on-USB console, no 1200-baud reflash; `tools/flash.sh` deleted. Diagnostics now come from the WiFi stats page.
- **Added a USB self-heal watchdog** (`main.c`): if USB makes no progress for ~5 s while CAN is active, `esp_restart()` — a wedge becomes a few-second blip instead of hours dark. Backstop only; see result.
- Added WiFi + HTTP stats/OTA + rollback at the same time, so future fixes push over the air.

**Result — 16 h soak under full load (everything on except one Axiom):**

| Check | Result |
|---|---|
| Continuous uptime | **60,127 s (16.7 h)**, one unbroken boot — no wedge, no reboot. The old firmware died at ~68 min. |
| Watchdog | **Never fired** → this is prevention (CDC removal), not recovery. |
| TX under load | `tx 1,392,924` frames, **`tx_failed 0`** — the AIS-bridge TX load that triggered the original wedge, handled flawlessly. |
| RX / errors | `rx 10,029,005`, `rx_overflow 0`, `bus_off 0`, `bus_err` ~3.5/min (benign). |
| WiFi | up the whole time (−49 dBm), stable IP. |
| Signal K | `pi_can0` streaming unbroken; no reconnect needed. |

Load note: the Signal K AIS→N2K bridge is a steady ~7 deltas/s (≈23 CAN frames/s TX) — not a flood; the gateway kept up easily. (The "network congestion detected" toast seen in the Signal K Data Browser during this is a **browser-side** admin-UI warning about its own WebSocket delta stream, unrelated to the bus or the gateway.)

**Flashing gotcha (Windows).** Flashed from a PC via USB (button download → `idf.py -p COM<n> flash`). After the flash the chip kept re-entering the ROM bootloader (still `303a:1001`) because the RTC force-download flag survives resets and esptool's RTS reset on the USB-Serial/JTAG port is finicky on Windows. Fix: `esptool --chip esp32s3 -p COM<n> --before default-reset --after hard-reset write-mem 0x6000812C 0`, then **unplug/replug** (a full power cycle clears both the RTC flag and the DTR/RTS line state).
