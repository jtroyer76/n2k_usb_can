# Roadmap

What's planned, in rough order. Design detail lives in [spec.md](spec.md); test history in [TESTING.md](TESTING.md). Nothing here is urgent — the adapter is in daily use on a live NMEA 2000 bus as it stands.

## v0.1.0 — first release

- **Settings on the device, with a setup hotspot.** WiFi SSID/password and the device password (which gates OTA) live in NVS. With nothing saved, an open `N2K-CAN-XXXX` hotspot serves a setup page; if the saved network is missing for 5 minutes, the hotspot returns, protected by the device password. `secrets.h` is optional, for development only.
- **mDNS**, so `n2k-can-gw.local` resolves.
- **Web interface** — status tiles, a live log console (RAM ring buffer), restart, and settings with firmware upload from the browser.
- **CI.** Builds with and without `secrets.h` on every pull request and push to `main`.
- **Release on tag.** A `v*` tag builds the image, checks the firmware version matches the tag, attaches a merged image (write at `0x0`) and an OTA image to a GitHub release, and deploys the web installer.
- **Web installer on GitHub Pages.** ESP Web Tools page for the first install: hold the button for download mode, click Install, replug.
- From this tag on, changes go through branches and pull requests rather than straight to `main`.

**Then:** run it on the boat for a week or two (watch `/api/status` and the console for restarts, falling memory, bus errors, WiFi drops), and test the paths not yet exercised — the locked fallback hotspot (boat WiFi off > 5 min), the web installer on a fresh AtomS3, browser upload, reinstall-with-erase. Clean → tag **1.0.0**.

## v0.1.1 — update from GitHub, simpler passwords

- **Online update.** Settings gets *Check for updates*: the gateway asks the GitHub API for this repo's newest release, shows "Running vX · vY is available" with the release notes, and *Install* downloads that release's `-update.bin` over HTTPS (`esp_https_ota`, ESP-IDF's built-in CA bundle) and installs it. The repo is fixed in the firmware, so it can only install official releases; the existing rollback still applies. Needs internet on the boat network at the time; the file upload stays as the fallback (no internet, test builds, or a future CA change the old firmware's bundle doesn't know). While the running firmware is a pre-release, the check includes pre-releases (GitHub's "latest" skips them).
- **Passwords by risk.** Being on the boat network is the trust boundary (as phone pairing is for Victron):

  | Action | Device password |
  |---|---|
  | Status, console | no |
  | Restart | no |
  | Install an update from GitHub | no — official releases only, rollback-protected |
  | Upload a firmware file | yes — could be anything |
  | Change WiFi or the device password | yes — could lock you out |

  `POST /update` (file upload / curl) keeps the `X-OTA-Key` check.

## Robustness

- **CAN RX-silence watchdog.** On large buses the ESP32 TWAI controller has been reported to enter a state where it still transmits but stops receiving, with no error flag set, after days of uptime ([ttlappalainen/NMEA2000_esp32#39](https://github.com/ttlappalainen/NMEA2000_esp32/issues/39)). Not yet confirmed on the ESP32-S3. Detect it (e.g. our own TX completing while RX stays silent for N seconds) and reset the TWAI node, mirroring the existing USB self-heal watchdog.
- **Host-side reconnect.** Signal K (canboatjs) does not re-attach robustly when `can0` disappears and reappears, so a gateway restart or OTA leaves Signal K without N2K data until it is restarted. Harden the SocketCAN reconnect in canboatjs, or document a systemd workaround.

## Later

- **Own USB PID.** Apply for a pid.codes PID and ship a udev rule for kernels without an upstream `gs_usb` entry (see spec §5). Until then the firmware uses the candleLight `1d50:606f`.
- **WiFi YD RAW over TCP**, for Signal K's "Yacht Devices" input and OpenCPN, alongside USB.
