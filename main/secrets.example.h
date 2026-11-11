// Optional, for development: copy to secrets.h (gitignored) and fill in. These are defaults
// for settings not yet saved on the device (NVS); they are saved on first boot, and after
// that the device's own settings win. Release builds have no secrets.h: WiFi is set up
// from the setup hotspot instead.
//
//   cp main/secrets.example.h main/secrets.h   # then edit
//
#pragma once

// Boat WiFi network the gateway joins (STA). SSID is case-sensitive.
#define WIFI_SSID   "your-ssid"
#define WIFI_PASS   "your-wifi-password"

// Secret required in the "X-OTA-Key" HTTP header to push a firmware update.
// Pick something long; it's the only thing gating who can reflash over WiFi.
#define OTA_KEY     "change-me"
