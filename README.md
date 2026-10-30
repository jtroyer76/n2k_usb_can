# n2k_usb_can

Open-source firmware that turns an **M5Stack AtomS3 Lite** (ESP32-S3) on an **M5Stack ATOMIC CAN Base** (isolated transceiver) into a **USB-to-CAN adapter** compatible with the Linux `gs_usb` driver (candleLight protocol). Plug it into a Raspberry Pi and it appears as `can0` — no serial bridge, no drivers.

Built for NMEA 2000 (250 kbit/s) with Signal K / canboatjs, but it is a general-purpose classic-CAN adapter.

**Status:** v1 builds; not yet tested on hardware. See [docs/spec.md](docs/spec.md).

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

The firmware owns the USB port, so the board has to be put into download mode by hand for each flash:

1. Hold the reset button (~2 s) until the internal green LED lights, then release (M5Stack's documented procedure).
2. `idf.py -p <port> flash`, or flash `build/merged.bin` at offset `0x0` from Chrome with [esptool-js](https://espressif.github.io/esptool-js/).
3. Press reset or replug to run the firmware.

## Raspberry Pi / Signal K

Bring `can0` up at 250 kbit/s on every boot with systemd-networkd:

```
# /etc/systemd/network/80-can.network   (mode 644)
[Match]
Name=can0

[CAN]
BitRate=250K
RestartSec=100ms
```

```
sudo systemctl enable --now systemd-networkd
```

Check with `ip -details -statistics link show can0` and `candump can0`.

Signal K: Server → Data Connections → NMEA 2000 → **Canbus (canboatjs)**, interface `can0`. If Signal K runs in Docker, the container needs `network_mode: host` — `can0` lives in the host network namespace.

## License

[Apache License 2.0](LICENSE)
