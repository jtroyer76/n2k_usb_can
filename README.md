# n2k_usb_can

Open-source firmware that turns an **M5Stack AtomS3 Lite** (ESP32-S3) on an **M5Stack ATOMIC CAN Base** (isolated transceiver) into a **USB-to-CAN adapter** compatible with the Linux `gs_usb` driver (candleLight protocol). Plug it into a Raspberry Pi and it appears as `can0` — no serial bridge, no drivers. It also joins WiFi so it can be **updated over the air**.

Built for NMEA 2000 (250 kbit/s) with Signal K / canboatjs, but it is a general-purpose classic-CAN adapter.

**Status:** deployed on a live NMEA 2000 bus (since 2026-11-01) as the Signal K N2K input on the boat "Nootka", replacing an Actisense-format-over-serial gateway. A USB wedge under sustained load was found and fixed on 2026-11-03 (removed the CDC console, added a self-heal watchdog) and WiFi + OTA were added; it then ran **16 h clean under full load** with no wedge. See [docs/TESTING.md](docs/TESTING.md) and [docs/spec.md](docs/spec.md).

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
- **WiFi STA** — joins the configured network on boot (credentials in gitignored `main/secrets.h`)
- **HTTP stats + OTA** — `GET /` plain-text status page; `POST /update` firmware upload (gated by an `X-OTA-Key` header), with **bootloader rollback** if a pushed image doesn't prove healthy
- **USB self-heal watchdog** — restarts the chip if USB stops making progress while CAN is active (a backstop; the underlying wedge was fixed by removing the CDC console)
- Status LED: dim red = no USB host, blue = idle, green = bus OK (bright flash on traffic), orange = error warning/passive, red blinking = bus-off, magenta blinking = identify

Not implemented: WiFi YD RAW over TCP, CAN FD, hardware timestamps, mDNS, Windows driverless install.

## Build

ESP-IDF **v6.1**.

```
idf.py set-target esp32s3
idf.py build
```

WiFi needs credentials: copy `main/secrets.example.h` to `main/secrets.h` (gitignored) and fill in `WIFI_SSID`, `WIFI_PASS`, and an `OTA_KEY`.

## Flash & update

**First flash (USB, one time):**

1. Hold the AtomS3 side button ~2 s until the internal green LED lights, then release — this enters ROM download mode (enumerates as `303a:1001`).
2. `idf.py -p <PORT> flash`.
3. If it keeps re-entering the bootloader after flashing (still `303a:1001`), the RTC force-download flag is stuck — clear it and power-cycle:
   ```
   esptool --chip esp32s3 -p <PORT> --before default-reset --after hard-reset write-mem 0x6000812C 0
   ```
   then unplug/replug.

**Every update after that: OTA over WiFi, no cable.** Find the gateway's IP (its DHCP hostname is `n2k-can-gw`; there is no mDNS responder, so `n2k-can-gw.local` does not resolve — use the router's lease list or an ARP scan), then:

```
curl -H "X-OTA-Key: <your key>" --data-binary @build/n2k_usb_can.bin http://<gateway-ip>/update
```

The device reboots into the new image as *pending-verify* and only keeps it if it proves healthy (USB passing data + CAN up) within a minute — otherwise the bootloader rolls back to the previous image.

## Status page

`GET /` returns plain text, reachable over WiFi even if USB has wedged:

```
n2k_usb_can gateway
fw <git> built <date>
partition ota_0  pending-verify no
uptime <s>
wifi up  rssi <dBm>  ip <ip>
usb mounted yes
can up  state <n>  tx_err <n>  rx_err <n>
rx <n>  tx <n>  tx_failed <n>  rx_overflow <n>  bus_err <n>  bus_off <n>
```

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
