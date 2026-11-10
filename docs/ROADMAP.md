# Roadmap

What's planned, in rough order. Design detail lives in [spec.md](spec.md); test history in [TESTING.md](TESTING.md). Nothing here is urgent — the adapter is in daily use on a live NMEA 2000 bus as it stands.

## Toward the first release (v0.1.0)

A public release can't ship today: WiFi credentials and the OTA key are compiled in from a gitignored `main/secrets.h`, so a generic binary would have no network and no update path. These steps, in order, fix that and publish the first release.

1. **Settings stored on the device, with a setup hotspot.** Move WiFi SSID/password and the OTA key out of `secrets.h` into NVS. With no WiFi configured (or after a bench reset), the gateway starts its own access point and serves a setup page to enter them. `secrets.h` goes away.
2. **On-device web interface.** Replace the plain-text stats page with:
   - **Status** — CAN state, counters, USB state, WiFi, firmware version, auto-refreshing.
   - **Log** — a RAM ring buffer of recent log lines (`/log`), so diagnostics don't need UART access.
   - **Settings** — the step 1 fields, editable after setup.
   - **Update** — firmware upload from the browser (same rollback-protected OTA path as `POST /update`).
3. **mDNS responder**, so `n2k-can-gw.local` resolves without hunting for the DHCP address.
4. **CI.** Build on every pull request; on a version tag, build and attach release assets to a GitHub release: the app image (for OTA) and a merged image at offset 0 (for first flash).
5. **Tag v0.1.0.** From this point, changes go through branches and pull requests rather than straight to `main`.
6. **Web flasher on GitHub Pages.** ESP Web Tools page + manifest (`chipFamily: "ESP32-S3"`, merged image at offset 0), linked from the README. First flash: hold the button for ROM download mode, the board enumerates as USB-Serial/JTAG, and the browser flashes it — no ESP-IDF install needed.

## Robustness

- **CAN RX-silence watchdog.** On large buses the ESP32 TWAI controller has been reported to enter a state where it still transmits but stops receiving, with no error flag set, after days of uptime ([ttlappalainen/NMEA2000_esp32#39](https://github.com/ttlappalainen/NMEA2000_esp32/issues/39)). Not yet confirmed on the ESP32-S3. Detect it (e.g. our own TX completing while RX stays silent for N seconds) and reset the TWAI node, mirroring the existing USB self-heal watchdog.
- **Host-side reconnect.** Signal K (canboatjs) does not re-attach robustly when `can0` disappears and reappears, so a gateway restart or OTA leaves Signal K without N2K data until it is restarted. Harden the SocketCAN reconnect in canboatjs, or document a systemd workaround.

## Later

- **Own USB PID.** Apply for a pid.codes PID and ship a udev rule for kernels without an upstream `gs_usb` entry (see spec §5). Until then the firmware uses the candleLight `1d50:606f`.
- **WiFi YD RAW over TCP**, for Signal K's "Yacht Devices" input and OpenCPN, alongside USB.
