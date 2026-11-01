# n2k_usb_can

Open-source firmware that turns an **M5Stack AtomS3 Lite** (ESP32-S3) on an **M5Stack ATOMIC CAN Base** (isolated transceiver) into a **USB-to-CAN adapter** compatible with the Linux `gs_usb` driver (candleLight protocol). Plug it into a Raspberry Pi and it appears as `can0` — no serial bridge, no drivers.

Built for NMEA 2000 (250 kbit/s) with Signal K / canboatjs, but it is a general-purpose classic-CAN adapter.

**Status:** v1 passes desk tests on a Raspberry Pi (loopback, 2000-frame burst, error reporting) and is **deployed on a live NMEA 2000 bus** (2026-11-01) as the Signal K N2K input on the boat "Nootka", replacing an Actisense-format-over-serial gateway. See [docs/TESTING.md](docs/TESTING.md) and [docs/spec.md](docs/spec.md).

## Hardware

| Part | Notes |
|---|---|
| M5Stack AtomS3 Lite (C124) | ESP32-S3, native USB |
| M5Stack ATOMIC CAN Base (A103) | Isolated CA-IS3050G. No termination resistor (correct for an NMEA 2000 drop). Powered from USB only |

CAN pins: TX = GPIO5, RX = GPIO6. Both parts are rated 0–40 °C — indoor use.

## What v1 does

- Enumerates as a gs_usb device (VID:PID `1d50:606f`, candleLight's default) → Linux binds `gs_usb` automatically → `can0`
- Bit timing from the host (any classic CAN bitrate the controller supports)
- TX echo when the controller finishes with the frame, not when it is queued
- Error frames for warning / passive / bus-off / restart, RX overflow flag, `GET_STATE` error counters, optional bus-error reporting
- Automatic bus-off recovery (the Linux gs_usb driver cannot restart a channel itself)
- Listen-only, loopback, one-shot, triple-sample modes
- USB serial console beside `can0` (`/dev/ttyACM0` on Linux): log output and the commands `stats`, `reboot`, `bootloader`
- Reflash over USB without the reset button: a 1200-baud touch on the console reboots into the ROM bootloader (`tools/flash.sh`)
- Status LED: dim red = no USB host, blue = idle, green = bus OK (bright flash on traffic), orange = error warning/passive, red blinking = bus-off, magenta blinking = identify

Not in v1: WiFi/YD RAW, web config, CAN FD, hardware timestamps, Windows driverless install.

## Build

ESP-IDF **v6.1**.

```
idf.py set-target esp32s3
idf.py build
```

Merged image for single-file flashing at offset `0x0`:

```
cd build
esptool --chip esp32s3 merge-bin -o merged.bin @flash_args
```

## Flash

**From Linux, no button** (the adapter must be running this firmware):

```
tools/flash.sh build/merged.bin
```

It opens the console at 1200 baud so the firmware reboots into the ROM bootloader, writes the image with esptool, then clears the force-download flag and resets back into the firmware. Debian's `esptool` package has no flasher stub: run it as `ESPTOOL_ARGS=--no-stub tools/flash.sh ...`. Needs access to the serial ports (`dialout` group or `sudo`).

**First flash, or recovery** (factory firmware, or a build that doesn't come up):

1. Hold the side (reset) button ~2 s until the internal green LED lights, then release (M5Stack's documented procedure).
2. `idf.py -p <port> flash`, or flash `build/merged.bin` at offset `0x0` from Chrome with [esptool-js](https://espressif.github.io/esptool-js/).
3. Tap the side button to run the firmware.

## Console

The second USB interface is a CDC-ACM serial port, `/dev/serial/by-id/usb-n2k_usb_can_*-if01` on Linux. It carries the firmware log once USB is up, and takes line commands:

| Command | Does |
|---|---|
| `stats` | Uptime, last reset reason, CAN state and error counters, frame counters |
| `reboot` | Restart the firmware |
| `bootloader` | Reboot into the ROM bootloader (same as the 1200-baud touch) |

Output is dropped while nothing has the port open, so the console never holds up CAN traffic.

## Raspberry Pi / Signal K

Bring `can0` up at 250 kbit/s on every boot with systemd-networkd:

```
# /etc/systemd/network/80-can.network   (mode 644)
[Match]
Name=can0

[CAN]
BitRate=250K
```

```
sudo systemctl enable --now systemd-networkd
```

> **Do not add `RestartSec=` to the `[CAN]` section.** It maps to the kernel's `restart-ms` (driver-side automatic bus-off restart), which this firmware does not support — the gs_usb driver rejects it with *"Device doesn't support restart from Bus Off"* (the firmware does its own bus-off recovery; see [docs/TESTING.md](docs/TESTING.md)). systemd-networkd treats that one rejected setting as a failed config and, after a few retries, **refuses to bring `can0` up at all** — so the interface stays `DOWN` with the bitrate set. Omitting the line is all that's needed; networkd's default `ActivationPolicy=up` brings the link up.

Check with `ip -details -statistics link show can0` and `candump can0`. One-shot alternative to networkd: `sudo ip link set up can0 type can bitrate 250000`.

Signal K: Server → Data Connections → NMEA 2000 → **Canbus (canboatjs)**, interface `can0`. If Signal K runs in Docker, the container needs `network_mode: host` — `can0` lives in the host network namespace.

## License

[Apache License 2.0](LICENSE)
