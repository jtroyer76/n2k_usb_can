# atoms3-can-gateway — Spec

Status: draft, pre-implementation (2026-10-27). Research notes behind this spec are summarized in [§9 Sources](#9-sources). Items marked **UNVERIFIED** are inferences that must be checked before or during implementation.

## 1. Goal

USB-powered, isolated USB-to-CAN adapter built from off-the-shelf M5Stack parts, presenting the Linux `gs_usb` (candleLight) protocol so it appears as a native SocketCAN interface (`can0`) with no host drivers. Primary use: NMEA 2000 (250 kbit/s) into Signal K on a Raspberry Pi. Open source, flashable from Chrome.

**Why:** replaces an Actisense-format-over-USB-serial gateway (original-ESP32 Atom Lite at 115200 baud). A byte stream without flow control can lose framing when the host stalls and decode garbage source addresses until restarted; it is also bandwidth-limited. SocketCAN carries whole frames — a host stall drops frames, never invents them.

### Non-goals (v1)

- CAN FD (the ESP32-S3 TWAI controller does not support it)
- NMEA 2000 protocol logic on the device (address claim etc. is done by canboatjs on the host)
- WiFi / YD RAW mode, web config page, browser OTA upload (v2 — see §11)
- Multiple CAN channels

## 2. Hardware

| Part | Facts | Source |
|---|---|---|
| M5Stack AtomS3 Lite (C124) | ESP32-S3FN8, 8 MB flash, no PSRAM. USB-C wired to the S3's native USB (no bridge chip). LED WS2812 on GPIO35, button GPIO41 (active low). 0–40 °C | M5Stack docs, AtomS3 v1.0 schematic, M5Unified |
| M5Stack ATOMIC CAN Base (A103) | CA-IS3050G isolated transceiver, B0505S-1W isolated DC-DC powered from the Atom's 5 V. **No 120 Ω termination.** **Cannot be powered from the CAN connector** (pin 3 NC). 0–40 °C | M5Stack docs + schematic |
| CAN pins on AtomS3 | **TX = GPIO5, RX = GPIO6** (Atom Lite equivalent: G22/G19). Some M5Stack prose docs have them reversed — code examples are authoritative | M5AtomS3 `examples/AtomBase/AtomCan/AtomCan.ino`, M5Stack AtomS3R tutorial code |
| CAN connector (HT3.96-4P) | CANH, CANL, NC, GND. The GND pin goes to a SHIELD net joined to the isolated bus ground only through 1 MΩ ∥ 1 nF/2 kV | Base schematic |

Implications:
- NMEA 2000 drop: CANH/CANL only is the expected wiring; evaluate whether to land NET-C or shield on the GND pin (**open**). No termination needed on a drop.
- Device is powered from the host USB; nothing is drawn from NET-S.
- Indoor (nav station) only — 0–40 °C rating.

**UNVERIFIED:** GPIO19/20 → USB D−/D+ routing (seen in schematic text, not drawing); B0505S-1W isolation rating; which physical button the documented download-mode procedure refers to.

### ESP32-S3 TWAI

The ESP32-S3 errata list has **no TWAI entries** (the original ESP32 has 12, incl. RX FIFO overrun lock-up and bus-off recovery issues). Not CAN FD capable — FD frames are treated as errors. Bus-off recovery requires an explicit recover call plus 129×11 recessive bits.

## 3. Prior art — what exists

| Project | What it gives us | License | Verdict |
|---|---|---|---|
| [wikilift/CandleLight_ESP32-firmware](https://github.com/wikilift/CandleLight_ESP32-firmware) | ESP32-S3 gs_usb on esp_tinyusb vendor class, ~470 lines. Host format, bittiming, mode, device config, BT const, identify | README says MIT but **no LICENSE file** | Reference only. 4 commits (Nov 2025), no timestamps/error frames, echo before TX completes, unlocked driver reinstall race, possibly wrong BRP limits, silent drops |
| [cherry-embedded/CherryUSB](https://github.com/cherry-embedded/CherryUSB) | gs_usb device class (stubs) + template with 1d50:606f and MS OS descriptors; has an ESP32-S2/S3 port | Apache-2.0 | Option for the USB stack — gs_usb class is thin; no CAN backend |
| [candle-usb/candleLight_fw](https://github.com/candle-usb/candleLight_fw) | Reference gs_usb device (STM32): request handling, feature set, descriptors, MS OS 1.0 WinUSB, DFU runtime interface | MIT | Protocol reference |
| [CANnectivity](https://github.com/CANnectivity/cannectivity) | Clean gs_usb implementation (Zephyr); got its own pid.codes PID upstreamed | Apache-2.0 | Protocol + PID-process reference |
| [wellenvogel/esp32-nmea2000](https://github.com/wellenvogel/esp32-nmea2000) | Supports AtomS3 + ATOMIC CAN base (`M5_CAN_KIT`) as an N2K→0183 gateway | GPL-2.0 | Pin/hardware reference only (GPL — don't copy code) |
| SLCAN projects (mintynet, Pacerino, raibisch) | ESP32 serial-line CAN | none | Not usable (no license; and slcan is serial anyway) |

**No existing gs_usb firmware for AtomS3 + ATOMIC CAN base was found.** That is the gap this project fills.

**Decision: write our own**, using candleLight_fw and CANnectivity as protocol references and wikilift as an ESP32-specific sketch. Do not copy wikilift code unless the author adds a LICENSE.

## 4. Firmware design

### Platform

- ESP-IDF v6.1 (current release), C.
- USB: **esp_tinyusb 3.x** (Espressif-maintained; vendor class with FIFO bypass; custom descriptors). CherryUSB is the fallback if esp_tinyusb's vendor path proves awkward.
- CAN: ESP-IDF TWAI driver. Choose between the new `esp_twai` node API and legacy `driver/twai.h` during implementation (legacy has `TWAI_TIMING_CONFIG_*` presets; new API sets bitrate numerically).

### USB descriptors

| Item | Value |
|---|---|
| Interface 0 | gs_usb: class/subclass/protocol 0xFF/0xFF/0xFF, bulk **EP IN 0x81, EP OUT 0x02** (candleLight layout; mainline and Pi 6.6/6.12 kernels read endpoints from descriptors, older ones hardcode) |
| Interfaces 1–2 | CDC-ACM (IAD) — status/console + reboot-to-bootloader (§6) |
| MS OS 1.0 descriptors | WinUSB compatible ID on interface 0 so Windows binds without Zadig (as candleLight) |
| VID:PID | see §5 |
| Serial string | from MAC (esp_tinyusb 3.x default) |

The kernel driver binds **interface 0 only**.

### gs_usb protocol

Required (probe/open fail without them):

| Request | Dir | Behaviour |
|---|---|---|
| HOST_FORMAT (0) | OUT | ACK; always little-endian |
| DEVICE_CONFIG (5) | IN | `icount=0` (one channel), sw/hw version |
| BT_CONST (4) | IN | feature flags + `fclk_can`, tseg1/tseg2/sjw/brp limits. **Values must match the S3 TWAI hardware exactly** — derive from the ESP32-S3 TRM / IDF TWAI HAL (**UNVERIFIED**: wikilift's 80 MHz, brp 1–128 step 1 is suspected wrong) |
| BITTIMING (1) | OUT | apply prop/phase1/phase2/sjw/brp to TWAI |
| MODE (2) | OUT | START (+flags) / RESET |

Data path: one `gs_host_frame` per USB transfer (20 bytes classic; 24 with timestamps). Use vendor TX buffer size 0 (FIFO bypass) or one transfer per frame — **never coalesce two frames into one packet**. RX frames use `echo_id = 0xFFFFFFFF`.

**TX echo (mandatory):** the host has 10 TX slots; every host frame must be echoed with its `echo_id` or the queue stalls after 10. Echo **on TWAI TX completion** (more correct than candleLight's echo-on-queue). Track in-flight echo IDs in FIFO order. On TX failure/abort, still release the slot — exact behaviour to match the kernel's expectations (**open**, check `gs_usb.c` handling).

Optional features — v1 set:

| Feature | v1 | Why |
|---|---|---|
| BERR_REPORTING + GET_STATE | yes | Bus errors and error counters visible in `ip -details link` / candump; essential for health monitoring |
| OVERFLOW flag on RX frames | yes | Report RX overruns instead of dropping silently |
| HW_TIMESTAMP (1 MHz) | yes | Cheap with esp_timer; useful for diagnostics; not needed by Signal K |
| LISTEN_ONLY, LOOP_BACK, ONE_SHOT | yes if TWAI maps cleanly | Bench testing; listen-only for safe side-by-side trials |
| IDENTIFY | yes | Blink the LED (`sw_version > 1` required by driver) |
| BUS_OFF_RECOVERY | yes | Automatic recovery, also host-requested |
| FD, TERMINATION, BT_CONST_EXT | no | Not applicable |

### Concurrency

- Single owner of the TWAI driver; bittiming/mode changes stop RX/TX tasks under a lock before reinstalling (avoid wikilift's race).
- RX task: TWAI → USB IN, bounded queue, overflow counted and flagged.
- TX path: USB OUT → TWAI TX queue; echo on completion.

### Status LED (GPIO35)

Distinct states for: no USB host / enumerated idle / bus active (blink on traffic) / bus error / bus-off / identify. Exact colours TBD.

### Counters

RX frames, TX frames, RX overflows, TX failures, bus errors, bus-off events, uptime — readable over the CDC interface.

## 5. USB VID:PID

| Option | Host setup | Notes |
|---|---|---|
| Reuse **1d50:606f** | None, any kernel | De-facto practice; candleLight_fw itself defaults to it |
| Reuse 1209:2323 | None | Registered to bytewerk.org for candleLight hardware |
| Own PID (pid.codes 0x1209 or openmoko 0x1d50) | udev `new_id` rule until upstreamed to `gs_usb.c` (CANnectivity did this) | Requires the public repo + LICENSE first |

**Decided:** use 1d50:606f; apply for a pid.codes PID once the repo is public and working; ship a udev rule (modelled on CANnectivity's `99-cannectivity.rules`) for kernels without the upstream entry.

## 6. Flashing & updates

- **First flash:** Chrome/Edge via ESP Web Tools (Web Serial) over the S3's USB-Serial/JTAG. Manifest `chipFamily: "ESP32-S3"`, one merged binary at **offset 0** (S3 bootloader is at 0x0). Build the merged image with `esptool --chip esp32s3 merge-bin -o merged.bin @flash_args`.
- **Problem:** once our firmware runs, TinyUSB owns the USB PHY — the USB-Serial/JTAG auto-reset path is gone, and a gs_usb-only device has no serial port for Web Serial to see.
- **Solution:** the CDC interface supports a **1200-baud touch** → firmware switches the PHY back to USB-Serial/JTAG, sets `RTC_CNTL_FORCE_DOWNLOAD_BOOT`, restarts into ROM download mode (enumerates as 303a:1001, which esptool-js handles natively). Model on arduino-esp32 `esp32-hal-tinyusb.c` (`usb_switch_to_cdc_jtag`, `usb_persist_restart`).
- **Fallback:** manual download mode — M5Stack: hold the reset button ~2 s until the internal green LED lights, release.
- **UNVERIFIED:** whether ESP Web Tools performs the 1200-baud touch itself; may need our own small flasher page (esptool-js) that does touch → re-select port → flash.
- Host GitHub Pages flasher page with the manifest.

## 7. Host setup (Raspberry Pi + Signal K)

1. Bring up can0 at boot — systemd-networkd:
   ```
   # /etc/systemd/network/80-can.network  (mode 644)
   [Match]
   Name=can0
   [CAN]
   BitRate=250K
   RestartSec=100ms
   ```
   `sudo systemctl enable --now systemd-networkd`. **UNVERIFIED:** coexistence with NetworkManager on current Pi OS — check `nmcli device`. One-shot alternative: `sudo ip link set up can0 type can bitrate 250000`.
2. Signal K: Server → Data Connections → NMEA 2000 → **Canbus (canboatjs)**, Interface `can0`. Optionally fix UniqueNumber.
3. **Docker:** SocketCAN interfaces live in the host network namespace — the Signal K container needs **`network_mode: host`** (Signal K's own compose file uses host networking). `privileged` should not be required for CAN alone (**UNVERIFIED**, untested).
4. Device must run in normal (not listen-only) mode — canboatjs transmits (address claim).
5. Verify with `candump can0` and `ip -details -statistics link show can0`.

## 8. Test plan

1. **Bench:** two adapters (or adapter + existing Atom gateway) on a short terminated bus; `cansend`/`candump` both directions; loopback mode; bitrate changes; unplug/replug.
2. **Echo/flow:** sustained TX bursts > 10 frames (echo slot handling); `cangen` load test; check overflow flag and counters.
3. **Errors:** short CANH/CANL, remove termination → bus errors / bus-off reported and recovered.
4. **Flashing:** first flash from Chrome; 1200-baud-touch reflash; manual-mode recovery.
5. **Boat, side by side:** new adapter in listen-only alongside the current Actisense gateway; compare frame counts and source addresses (expect identical real sources, no phantoms). Then swap Signal K to can0 and retire the old input.
6. **Soak:** days of uptime with chartplotters, VHF and stereo powered (heavier bus load).

## 9. Open decisions

1. ~~License~~ — **decided: Apache-2.0** (2026-10-27).
2. ~~VID:PID~~ — **decided: 1d50:606f for now**; own PID later per §5.
3. **NMEA 2000 wiring** — CANH/CANL only, or land shield/NET-C on the base's GND pin?
4. **Echo-on-failure semantics** — confirm against `gs_usb.c`.
5. **BT_CONST values** — derive from TRM.
6. **TWAI API** — new `esp_twai` vs legacy.

## 10. Sources

- Linux `drivers/net/can/usb/gs_usb.c` (mainline; Raspberry Pi `rpi-6.6.y`, `rpi-6.12.y`)
- https://github.com/candle-usb/candleLight_fw — `src/usbd_gs_can.c`, `src/can/bxcan.c`, `include/gs_usb.h`, `include/config.h`
- https://github.com/CANnectivity/cannectivity — `99-cannectivity.rules`
- https://github.com/wikilift/CandleLight_ESP32-firmware
- https://github.com/cherry-embedded/CherryUSB — `class/gs_usb/`, `demo/gs_usb_template.c`
- https://github.com/wellenvogel/esp32-nmea2000 — `lib/hardware/GwM5Base.h`
- https://docs.m5stack.com/en/core/AtomS3%20Lite, https://docs.m5stack.com/en/atom/Atomic%20CAN%20Base, https://docs.m5stack.com/en/arduino/projects/atomic/atomic_can_base
- https://github.com/m5stack/M5AtomS3/blob/main/examples/AtomBase/AtomCan/AtomCan.ino
- https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32s3/02-errata-summary/index.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/twai.html
- https://github.com/espressif/esp-usb/tree/master/device/esp_tinyusb
- https://github.com/espressif/arduino-esp32 — `cores/esp32/esp32-hal-tinyusb.c`, `cores/esp32/USBCDC.cpp`
- https://esphome.github.io/esp-web-tools/, https://github.com/espressif/esptool-js
- https://pid.codes/howto/, https://github.com/openmoko/openmoko-usb-oui
- https://www.freedesktop.org/software/systemd/man/latest/systemd.network.html
- https://github.com/SignalK/signalk-server — `docker/docker-compose.yml`, `docker/Dockerfile_rel`, admin UI `BasicProvider.tsx`, `packages/streams/src/simple.ts`

## 11. Scope as built (v1) and v2 plan

**v1 (2026-10-27, builds, untested on hardware)** — kept deliberately simple:
- gs_usb on interface 0 only (no CDC interface). Reflash via manual download mode (§6 fallback).
- Advertised features: listen-only, loopback, one-shot, triple-sample, identify, bus-error reporting, get-state. Echo on TWAI TX completion (success or failure — the driver has no failure echo; failures surface as error frames). Automatic bus-off recovery in firmware.
- Deferred from §4: hardware timestamps, MS OS descriptors (Windows), CDC console + 1200-baud reboot-to-bootloader.
- Partition table already has two OTA slots, and the CAN layer (`can_bus.c`) produces transport-neutral frames, so v2 needs no repartitioning or CAN rework.

**v2 (proposed):**
- WiFi + YD RAW over TCP (Signal K "Yacht Devices" input, OpenCPN, etc.), able to run alongside USB.
- Web page: WiFi credentials, mode (auto / USB only / WiFi only), firmware upload (OTA). Password-protected — the device can transmit onto the bus.
- CDC interface with reboot-to-bootloader, if browser OTA doesn't make it redundant.
