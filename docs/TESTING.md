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
