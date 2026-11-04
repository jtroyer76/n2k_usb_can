# n2k_usb_can — working notes

Firmware that turns an **M5Stack AtomS3 Lite (ESP32-S3) + ATOMIC CAN Base** into a Linux `gs_usb` (candleLight) USB-to-CAN adapter — it appears as `can0` on a Raspberry Pi with no host drivers. Built for NMEA 2000 (250 kbit/s) into Signal K, but it's a general classic-CAN adapter. Apache-2.0, ESP-IDF **v6.1**, target esp32s3.

## Current state (as of 2026-11-04) — what's actually built and running

Deployed on the live NMEA 2000 bus on the boat "Nootka" (Pi `nootcore`) as Signal K's N2K input (`pi_can0`). All of this is **implemented and on the device**:

- **gs_usb / candleLight** on a single USB interface (VID:PID `1d50:606f`). Host sets bit timing; TX echo on TWAI completion; error frames, RX-overflow flag, GET_STATE counters, bus-error reporting; listen-only / loopback / one-shot / triple-sample / identify; automatic bus-off recovery.
- **WiFi STA** — joins the boat network on boot (`net_wifi.c`). Credentials live in **gitignored `main/secrets.h`** (copy `main/secrets.example.h`).
- **HTTP server** (`net_ota.c`): `GET /` plain-text stats page; `POST /update` firmware OTA, gated by the `X-OTA-Key` header. This is how you update it now — **over WiFi, no cable.**
- **OTA rollback health-check** (`ota_health.c`): an OTA'd image boots pending-verify and only marks itself valid after USB is genuinely passing data + CAN healthy for 60 s; otherwise the bootloader reverts. Dual-OTA partition table (`partitions.csv`) + `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
- **USB self-heal watchdog** (in `main.c`): `esp_restart()` if USB stops making progress while CAN is active. A backstop — see the wedge history below.

**Removed 2026-11-03: the CDC-ACM serial console.** It was the root cause of a USB wedge (see docs/TESTING.md 2026-11-04). The firmware is now a single gs_usb interface; logs go to UART0 (not exposed), never into a USB endpoint. Consequence: there's no on-USB console and no 1200-baud-touch reflash anymore — `tools/flash.sh` is gone. Diagnostics come from the WiFi stats page; reflash is button-download (first time) then OTA.

## Source layout (`main/`)

- `main.c` — app_main loop (LED, stats, the USB-wedge watchdog, ota_health poll), brings up CAN + USB + WiFi + OTA.
- `gs_usb_dev.c/.h` — TinyUSB gs_usb device: descriptors (single vendor interface), control requests, bulk IN/OUT, USB-progress timestamp for the watchdog.
- `gs_usb_proto.h` — gs_usb wire protocol (matches the Linux `gs_usb.c` driver).
- `can_bus.c/.h` — TWAI (esp_twai node API): RX/TX, echo slots, error/state callbacks, bus-off recovery.
- `net_wifi.c/.h` — esp_wifi STA + reconnect.
- `net_ota.c/.h` — esp_http_server: stats + OTA upload.
- `ota_health.c/.h` — pending-verify → mark-valid-or-rollback state machine.
- `status_led.c/.h` — WS2812 status LED.
- `secrets.h` (gitignored) — `WIFI_SSID`, `WIFI_PASS`, `OTA_KEY`.

## Build / flash / update

```
idf.py set-target esp32s3
idf.py build
```

**First flash (USB, one time):** hold the AtomS3 side button ~2 s (internal green LED) → ROM download mode → `idf.py -p <PORT> flash`. If after flashing it keeps booting back into the bootloader (still enumerates as `303a:1001`), the RTC force-download flag is stuck — clear it: `esptool --chip esp32s3 -p <PORT> --before default-reset --after hard-reset write-mem 0x6000812C 0`, then **power-cycle** (unplug/replug) to be sure.

**Updates after that: OTA over WiFi** (no cable):
```
curl -H "X-OTA-Key: <key from secrets.h>" --data-binary @build/n2k_usb_can.bin http://<gateway-ip>/update
```
A bad image auto-rolls-back (health-check). The gateway's IP: it registers its DHCP hostname `n2k-can-gw` (no mDNS responder yet, so `n2k-can-gw.local` does NOT resolve — find the IP from the router or an ARP scan).

## Host (Raspberry Pi / Signal K)

`can0` up at boot via systemd-networkd `/etc/systemd/network/80-can.network` with `BitRate=250K` — **do NOT add `RestartSec=`** (this firmware refuses `restart-ms`; networkd then refuses to bring the link up). Signal K: NMEA 2000 → Canbus (canboatjs), interface `can0`. Note: Signal K does **not** auto-reconnect robustly if `can0` disappears/reappears — a gateway restart needs a Signal K restart (or canboatjs reconnect hardening) to re-attach.

## What's next (ideas, none urgent)

- **mDNS responder** (so `n2k-can-gw.local` resolves) + a `/log` ring-buffer endpoint (real log lines over the web, not just counters). Both pushable via OTA.
- WiFi YD RAW over TCP (the original v2 idea — Signal K "Yacht Devices" input / OpenCPN).
- Harden the host side: canboatjs SocketCAN reconnect so a gateway restart doesn't leave Signal K dark.

## Conventions

Conventional Commits (`feat:`, `fix:`, `docs:`…). No AI attribution in commits. Public repo — keep it a general product (no boat name / Pi hostname / secrets in committed files; "tested on a boat" is fine). Commits straight to `main` until the first release tag.
