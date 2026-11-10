# n2k_usb_can — agent instructions

Firmware that turns an **M5Stack AtomS3 Lite (ESP32-S3) + ATOMIC CAN Base** into a Linux `gs_usb` (candleLight) USB-to-CAN adapter — it appears as `can0` on a Raspberry Pi with no host drivers. Built for NMEA 2000 (250 kbit/s) into Signal K, but it's a general classic-CAN adapter. Apache-2.0, ESP-IDF **v6.1**, target esp32s3.

- Design: `docs/spec.md` (§11 is the scope as built). Test history: `docs/TESTING.md`.
- **Planned work goes in `docs/ROADMAP.md`** — not in this file. Keep this file to how to work in the repo.

## Architecture constraints

- **Single gs_usb vendor interface** (VID:PID `1d50:606f`). There is deliberately **no CDC-ACM console**: it caused a USB wedge (logs piling into an unread endpoint stalled the stack — see TESTING.md 2026-11-04). Never route logs or anything else into a USB endpoint; logs go to UART0 (not exposed). Diagnostics come from the WiFi stats page.
- The **USB self-heal watchdog** in `main.c` (`esp_restart()` if USB stops making progress while CAN is active) is a backstop, not the fix — don't lean on it.
- **OTA rollback** (`ota_health.c`): an OTA'd image boots pending-verify and only marks itself valid after USB is passing data + CAN healthy for 60 s. Changes that delay USB or CAN bring-up can trip a rollback.
- The firmware refuses `restart-ms` (it does bus-off recovery itself).

## Source layout (`main/`)

- `main.c` — app_main loop (LED, stats, USB-wedge watchdog, ota_health poll), brings up CAN + USB + WiFi + OTA.
- `gs_usb_dev.c/.h` — TinyUSB gs_usb device: descriptors, control requests, bulk IN/OUT, USB-progress timestamp for the watchdog.
- `gs_usb_proto.h` — gs_usb wire protocol (matches the Linux `gs_usb.c` driver).
- `can_bus.c/.h` — TWAI (esp_twai node API): RX/TX, echo slots, error/state callbacks, bus-off recovery. No acceptance filter — every frame is passed.
- `net_wifi.c/.h` — esp_wifi STA + reconnect.
- `net_ota.c/.h` — esp_http_server: `GET /` stats, `POST /update` OTA gated by `X-OTA-Key`.
- `ota_health.c/.h` — pending-verify → mark-valid-or-rollback state machine.
- `status_led.c/.h` — WS2812 status LED.
- `secrets.h` (gitignored, required to build) — `WIFI_SSID`, `WIFI_PASS`, `OTA_KEY`. Copy from `secrets.example.h`.

## Build / flash / update

```
idf.py set-target esp32s3
idf.py build
```

**First flash (USB):** hold the AtomS3 side button ~2 s (internal green LED) → ROM download mode → `idf.py -p <PORT> flash`. If it keeps booting back into the bootloader (still enumerates as `303a:1001`), the RTC force-download flag is stuck — clear it: `esptool --chip esp32s3 -p <PORT> --before default-reset --after hard-reset write-mem 0x6000812C 0`, then **power-cycle**.

**Updates: OTA over WiFi:**
```
curl -H "X-OTA-Key: <key from secrets.h>" --data-binary @build/n2k_usb_can.bin http://<gateway-ip>/update
```
DHCP hostname is `n2k-can-gw`; there's no mDNS responder yet, so `n2k-can-gw.local` does not resolve — find the IP from the router or an ARP scan. An OTA restarts the gateway, so `can0` drops and returns; Signal K then needs a restart to re-attach.

## Host (Raspberry Pi / Signal K)

`can0` up at boot via systemd-networkd `/etc/systemd/network/80-can.network` with `BitRate=250K` — **do NOT add `RestartSec=`** (the firmware refuses `restart-ms`, and networkd then refuses to bring the link up). Signal K: NMEA 2000 → Canbus (canboatjs), interface `can0`.

## Conventions

- Conventional Commits (`feat:`, `fix:`, `docs:`…). No AI attribution in commits.
- **Public repo — keep it a general product.** No boat name, host names, IP addresses, Signal K connection names, or secrets in committed files. "Tested on a live NMEA 2000 bus" is fine.
- Commits straight to `main` until the first release tag (see ROADMAP); branches + PRs after that.
