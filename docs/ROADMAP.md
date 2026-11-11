# Roadmap

What's planned, in rough order. Design detail lives in [spec.md](spec.md); test history in [TESTING.md](TESTING.md). Nothing here is urgent — the adapter is in daily use on a live NMEA 2000 bus as it stands.

## v0.1.0 — first release

- **Settings on the device, with a setup hotspot.** WiFi SSID/password and the device password (which gates OTA) live in NVS. With nothing saved, an open `N2K-CAN-XXXX` hotspot serves a setup page; if the saved network is missing for 5 minutes, the hotspot returns, protected by the device password. `secrets.h` is optional, for development only.
- **mDNS**, so `n2k-can-gw.local` resolves.
- **CI.** Builds with and without `secrets.h` on every pull request and push to `main`.
- **Release on tag.** A `v*` tag builds the image, checks the firmware version matches the tag, attaches a merged image (write at `0x0`) and an OTA image to a GitHub release, and deploys the web installer.
- **Web installer on GitHub Pages.** ESP Web Tools page for the first install: hold the button for download mode, click Install, replug.
- From this tag on, changes go through branches and pull requests rather than straight to `main`.

## Web interface

- Replace the plain-text stats page with an auto-refreshing status page.
- **Log** — a RAM ring buffer of recent log lines (`/log`), so diagnostics don't need UART access.
- **Update** — firmware upload from the browser, on the same rollback-protected OTA path as `POST /update`.

## Robustness

- **CAN RX-silence watchdog.** On large buses the ESP32 TWAI controller has been reported to enter a state where it still transmits but stops receiving, with no error flag set, after days of uptime ([ttlappalainen/NMEA2000_esp32#39](https://github.com/ttlappalainen/NMEA2000_esp32/issues/39)). Not yet confirmed on the ESP32-S3. Detect it (e.g. our own TX completing while RX stays silent for N seconds) and reset the TWAI node, mirroring the existing USB self-heal watchdog.
- **Host-side reconnect.** Signal K (canboatjs) does not re-attach robustly when `can0` disappears and reappears, so a gateway restart or OTA leaves Signal K without N2K data until it is restarted. Harden the SocketCAN reconnect in canboatjs, or document a systemd workaround.

## Later

- **Own USB PID.** Apply for a pid.codes PID and ship a udev rule for kernels without an upstream `gs_usb` entry (see spec §5). Until then the firmware uses the candleLight `1d50:606f`.
- **WiFi YD RAW over TCP**, for Signal K's "Yacht Devices" input and OpenCPN, alongside USB.
