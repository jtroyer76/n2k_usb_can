#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

// Development builds: secrets.h stands in for settings not saved yet (secrets.example.h).
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef OTA_KEY
#define OTA_KEY ""
#endif

#define NS          "n2kcan"
#define GEN_PW_LEN  12
// No 0/O, 1/l/i: the password is read off a phone screen and typed.
static const char PW_CHARS[] = "abcdefghjkmnpqrstuvwxyz23456789";

static const char *TAG = "settings";
static settings_t s_set;
static SemaphoreHandle_t s_lock;

// Reads `key`; if it isn't saved, uses `fallback` and saves that (when non-empty), so a
// value from secrets.h survives a later image built without it.
static void load_str(nvs_handle_t h, const char *key, char *out, size_t len,
                     const char *fallback, bool *dirty)
{
    size_t n = len;
    if (h && nvs_get_str(h, key, out, &n) == ESP_OK) {
        return;
    }
    snprintf(out, len, "%s", fallback);
    if (h && fallback[0]) {
        nvs_set_str(h, key, out);
        *dirty = true;
    }
}

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        h = 0;
    }
    bool dirty = false;
    load_str(h, "wifi_ssid", s_set.wifi_ssid, sizeof(s_set.wifi_ssid), WIFI_SSID, &dirty);
    load_str(h, "wifi_pass", s_set.wifi_pass, sizeof(s_set.wifi_pass), WIFI_PASS, &dirty);
    load_str(h, "password", s_set.password, sizeof(s_set.password), OTA_KEY, &dirty);
    if (!s_set.password[0]) {   // first boot: make one up, and keep it
        for (int i = 0; i < GEN_PW_LEN; i++) {
            s_set.password[i] = PW_CHARS[esp_random() % (sizeof(PW_CHARS) - 1)];
        }
        s_set.password[GEN_PW_LEN] = '\0';
        if (h) {
            nvs_set_str(h, "password", s_set.password);
            dirty = true;
        }
        ESP_LOGI(TAG, "device password made up (shown on the setup page)");
    }
    if (h) {
        if (dirty) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "WiFi: %s", s_set.wifi_ssid[0] ? s_set.wifi_ssid : "(none saved: setup hotspot)");
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_set;
    xSemaphoreGive(s_lock);
}

const char *settings_check(const settings_t *s)
{
    size_t wp = strlen(s->wifi_pass), pw = strlen(s->password);
    if (!s->wifi_ssid[0]) {
        return "Choose a WiFi network";
    }
    if (wp && (wp < 8 || wp > 63)) {
        return "A WiFi password is 8 to 63 characters";
    }
    if (pw < SETTINGS_PW_MIN || pw > 63) {
        return "The device password must be 8 to 63 characters";
    }
    for (const char *c = s->password; *c; c++) {
        if (*c < '!' || *c > '~') {
            return "The device password: letters, digits and symbols only (no spaces)";
        }
    }
    return NULL;
}

bool settings_save(const settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(h, "wifi_ssid", s->wifi_ssid) == ESP_OK &&
              nvs_set_str(h, "wifi_pass", s->wifi_pass) == ESP_OK &&
              nvs_set_str(h, "password", s->password) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_set = *s;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "saved: %s", ok ? "ok" : "failed");
    return ok;
}

// Same time whatever the input, so the password can't be guessed a character at a time.
bool settings_password_ok(const char *given)
{
    settings_t s;
    settings_get(&s);
    size_t n = strlen(s.password);
    if (!n || strlen(given) != n) {
        return false;
    }
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)given[i] ^ (unsigned char)s.password[i];
    }
    return diff == 0;
}
