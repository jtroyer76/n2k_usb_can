// Per-device settings, kept in NVS: the WiFi network to join and the device password
// (which gates firmware updates and changing these settings). Set from the setup page
// (net_setup.c). Nothing has to be compiled in: a build with main/secrets.h (optional,
// gitignored) uses it for any setting not saved yet, and saves it on first boot.
#pragma once

#include <stdbool.h>

#define SETTINGS_SSID_LEN     33   // 32 + NUL (802.11 limit)
#define SETTINGS_WIFI_PW_LEN  65   // WPA2: 8..63, or "" for an open network
#define SETTINGS_PW_MIN       8    // device password (also the fallback hotspot's WPA2 key)
#define SETTINGS_PW_LEN       64

typedef struct {
    char wifi_ssid[SETTINGS_SSID_LEN];   // "" = none saved: start the setup hotspot
    char wifi_pass[SETTINGS_WIFI_PW_LEN];
    char password[SETTINGS_PW_LEN];      // device password; made up on first boot if none
} settings_t;

// Loads the settings. Call after nvs_flash_init.
void settings_init(void);

void settings_get(settings_t *out);

// Saves all fields (they apply after a restart). False on an NVS error.
bool settings_save(const settings_t *s);

// Checks settings before they're saved; NULL if fine, else what's wrong.
const char *settings_check(const settings_t *s);

// Constant-time comparison against the device password.
bool settings_password_ok(const char *given);
