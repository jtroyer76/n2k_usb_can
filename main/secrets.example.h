// Copy to secrets.h (gitignored) and fill in. secrets.h is REQUIRED to build.
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
