# n2k_usb_can

Open-source firmware that turns an **M5Stack AtomS3 Lite** (ESP32-S3) on an **M5Stack ATOMIC CAN Base** (isolated transceiver) into a **USB-to-CAN adapter** compatible with the Linux `gs_usb` driver (candleLight protocol). Plug it into a Raspberry Pi and it appears as `can0` — no serial bridge, no drivers. It also joins WiFi so it can be **updated over the air**.

Built for NMEA 2000 (250 kbit/s) with Signal K / canboatjs, but it is a general-purpose classic-CAN adapter.

**Status:** deployed on a live NMEA 2000 bus (since 2026-11-01) as a Signal K N2K input on a boat, replacing an Actisense-format-over-serial gateway. A USB wedge under sustained load was found and fixed on 2026-11-03 (removed the CDC console, added a self-heal watchdog) and WiFi + OTA were added; it then ran **16 h clean under full load** with no wedge. See [docs/TESTING.md](docs/TESTING.md), [docs/spec.md](docs/spec.md), and what's planned in [docs/ROADMAP.md](docs/ROADMAP.md).

## Hardware

| Part | Notes |
|---|---|
| M5Stack AtomS3 Lite (C124) | ESP32-S3, native USB |
| M5Stack ATOMIC CAN Base (A103) | Isolated CA-IS3050G. No termination resistor (correct for an NMEA 2000 drop). Powered from USB only |

CAN pins: TX = GPIO5, RX = GPIO6. Both parts are rated 0–40 °C — indoor use.

## What it does

- Enumerates as a **single-interface** gs_usb device (VID:PID `1d50:606f`, candleLight's default) → Linux binds `gs_usb` automatically → `can0`
- Bit timing from the host (any classic CAN bitrate the controller supports)
- TX echo when the controller finishes with the frame, not when it is queued
- Error frames for warning / passive / bus-off / restart, RX overflow flag, `GET_STATE` error counters, optional bus-error reporting
- Automatic bus-off recovery (the Linux gs_usb driver cannot restart a channel itself)
- Listen-only, loopback, one-shot, triple-sample modes
- **WiFi setup on the device** — with no network saved it starts an open hotspot (`N2K-CAN-XXXX`) with a setup page; settings live in NVS, nothing is compiled in. If the saved network is missing for 5 minutes the hotspot comes back, protected by the device password
- **Web interface** — status tiles, a live log console, settings, and firmware upload (see [Web interface](#web-interface)); updates are gated by the device password and protected by **bootloader rollback** if a new image doesn't prove healthy
- **mDNS** — `n2k-can-gw.local`
- **USB self-heal watchdog** — restarts the chip if USB stops making progress while CAN is active (a backstop; the underlying wedge was fixed by removing the CDC console)
- Status LED: dim red = no USB host, blue = idle, green = bus OK (bright flash on traffic), orange = error warning/passive, red blinking = bus-off, magenta blinking = identify

Not implemented: WiFi YD RAW over TCP, CAN FD, hardware timestamps, Windows driverless install.

## Build

ESP-IDF **v6.1**.

```
idf.py set-target esp32s3
idf.py build
```

WiFi is set up on the device (see [WiFi setup](#wifi-setup)), so a plain build needs nothing else. For development you can pre-fill the settings: copy `main/secrets.example.h` to `main/secrets.h` (gitignored) and fill in `WIFI_SSID`, `WIFI_PASS` and `OTA_KEY` (the device password). They're used for any setting not saved on the device yet, and saved on first boot — after that, the device's saved settings win.

## Install a release (browser, no toolchain)

**[Web installer](https://jtroyer76.github.io/n2k_usb_can/)** — Chrome or Edge on a computer. Hold the AtomS3 side button ~2 s first (download mode), then click Install. Unplug and replug when it's done. Each [GitHub release](https://github.com/jtroyer76/n2k_usb_can/releases) also has `n2k_usb_can-<version>-full.bin` (write at `0x0` on a fresh board) and `n2k_usb_can-<version>-update.bin` (for updates over WiFi):

```
esptool --chip esp32s3 -p <PORT> write-flash 0x0 n2k_usb_can-<version>-full.bin
```

## Flash & update (from source)

**First flash (USB, one time):**

1. Hold the AtomS3 side button ~2 s until the internal green LED lights, then release — this enters ROM download mode (enumerates as `303a:1001`).
2. `idf.py -p <PORT> flash`.
3. If it keeps re-entering the bootloader after flashing (still `303a:1001`), the RTC force-download flag is stuck — clear it and power-cycle:
   ```
   esptool --chip esp32s3 -p <PORT> --before default-reset --after hard-reset write-mem 0x6000812C 0
   ```
   then unplug/replug.

**Every update after that: OTA over WiFi, no cable** (once WiFi is set up):

```
curl -H "X-OTA-Key: <device password>" --data-binary @build/n2k_usb_can.bin http://n2k-can-gw.local/update
```

If `.local` names don't resolve on your machine, use the gateway's IP from the router's lease list (DHCP hostname `n2k-can-gw`).

The device reboots into the new image as *pending-verify* and only keeps it if it proves healthy (USB passing data + CAN up) within a minute — otherwise the bootloader rolls back to the previous image.

## WiFi setup

1. Power the gateway. With no WiFi saved, it starts its own open WiFi, **N2K-CAN-XXXX**.
2. Join it with a phone. The setup page usually opens by itself; if not, open `http://192.168.4.1/setup`.
3. Choose the boat's network and enter its password. Note the **device password** shown on the page, or set your own (8–63 characters): it gates firmware updates and changing settings later. It's shown only during this first setup.
4. Save. The gateway restarts and joins the network.

Later, change settings at `http://n2k-can-gw.local/setup` (needs the current device password). If the saved network is missing for 5 minutes, the hotspot comes back, protected by the device password as its WiFi password, and goes off again after 5 minutes with no phone on it. Forgotten the device password? Reinstall with "erase" from the web installer and set it up again.

## Web interface

Open `http://n2k-can-gw.local/` on the boat network. Reachable over WiFi even if USB has wedged.

- **Status** — tiles for the CAN bus (state, bitrate, error counters), traffic (rx/tx per second), errors (bus errors per minute, RX overflows, TX failures, bus-off), USB host, WiFi, uptime and memory, refreshed every 2 s. The firmware version is in the header.
- **Console** — recent log lines, scrolling live, with pause and auto-scroll. Kept in RAM (the last 160 lines); logs never go into a USB endpoint.
- **Restart** — restarts the gateway (`can0` drops for a few seconds).
- **Settings** (`/setup`) — WiFi network, device password, and **firmware update**: shows the running version, and uploads a release's `-update.bin`.

For scripts: `GET /api/status` (JSON), `GET /api/log?since=N` (JSON), `POST /update` (firmware, `X-OTA-Key` header).

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

Signal K: Server → Data Connections → NMEA 2000 → **Canbus (canboatjs)**, interface `can0`. If Signal K runs in Docker, the container needs `network_mode: host` — `can0` lives in the host network namespace. Note: Signal K does not robustly auto-reconnect if `can0` disappears and returns (e.g. after a device restart) — restart Signal K to re-attach.

## License

[Apache License 2.0](LICENSE)
