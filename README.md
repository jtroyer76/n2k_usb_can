# atoms3-can-gateway

Open-source firmware that turns an **M5Stack AtomS3 Lite** (ESP32-S3) on an **M5Stack ATOMIC CAN Base** (isolated transceiver) into a **USB-to-CAN adapter** compatible with the Linux `gs_usb` driver (candleLight protocol). Plug it into a Raspberry Pi and it appears as `can0` — no serial bridge, no drivers.

Built for NMEA 2000 (250 kbit/s) with Signal K / canboatjs, but it is a general-purpose CAN adapter.

**Status:** planning. See [docs/spec.md](docs/spec.md).
