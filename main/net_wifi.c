#include "net_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "captive_dns.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "settings.h"

static const char *TAG = "wifi";

static volatile bool s_connected;
static volatile bool s_setup;          // hotspot on; don't try the saved network
static bool s_setup_open;
static bool s_have_saved;
static char s_saved[33];
static char s_ap_ssid[33];
static char s_nets[SETUP_MAX_NETS][33];
static int s_net_count;
static SemaphoreHandle_t s_lock;
static esp_netif_t *s_netif;
static int64_t s_down_since_us;        // saved network not joined since (0 = joined)

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_saved && !s_setup) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected || !s_down_since_us) {
            s_down_since_us = esp_timer_get_time();
        }
        s_connected = false;
        if (!s_setup) {
            esp_wifi_connect();     // the radio drops occasionally; keep retrying
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_down_since_us = 0;
    }
}

// What's in range, for the setup page. Done before the hotspot comes on: a scan hops channels.
static void scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 10 && err != ESP_OK; i++) {   // a connect attempt may still be running
        err = esp_wifi_scan_start(&sc, true);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    uint16_t n = 24;
    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * n);
    if (err != ESP_OK || !recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        n = 0;
    }
    if (!n) {
        esp_wifi_clear_ap_list();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_net_count = 0;
    for (int i = 0; i < n && s_net_count < SETUP_MAX_NETS; i++) {   // strongest first
        const char *name = (const char *)recs[i].ssid;
        bool dup = !name[0];
        for (int j = 0; j < s_net_count && !dup; j++) {
            dup = strcmp(s_nets[j], name) == 0;
        }
        if (!dup) {
            snprintf(s_nets[s_net_count++], sizeof(s_nets[0]), "%.32s", name);
        }
    }
    xSemaphoreGive(s_lock);
    free(recs);
    ESP_LOGI(TAG, "scan: %d networks", s_net_count);
}

static void setup_on(void)
{
    settings_t set;
    settings_get(&set);
    size_t pw = strlen(set.password);
    // Open only for a first setup. A saved network means the gateway is someone's: lock the
    // hotspot with the device password (always 8..63 when set from the setup page).
    s_setup_open = !s_have_saved || pw < 8 || pw > 63;

    s_setup = true;
    esp_wifi_disconnect();
    scan();
    wifi_config_t ap = { 0 };
    ap.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(ap.ap.ssid, s_ap_ssid, ap.ap.ssid_len);
    if (s_setup_open) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        memcpy(ap.ap.password, set.password, pw);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    captive_dns_start();
    ESP_LOGI(TAG, "setup hotspot on: %s (%s)", s_ap_ssid,
             s_setup_open ? "open" : "device password");
}

static void setup_off(void)
{
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_setup = false;
    s_down_since_us = esp_timer_get_time();
    esp_wifi_connect();
    ESP_LOGI(TAG, "setup hotspot off (no phone), trying %s", s_saved);
}

static int phones(void)
{
    wifi_sta_list_t list;
    return s_setup && esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

static void wifi_task(void *arg)
{
    int64_t idle_since = 0;   // hotspot on with no phone since
    bool start = !s_have_saved;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (!s_setup && s_have_saved && !s_connected && s_down_since_us &&
            now - s_down_since_us > (int64_t)SETUP_AFTER_S * 1000000) {
            ESP_LOGW(TAG, "%s not joined for %d s", s_saved, SETUP_AFTER_S);
            start = true;
        }
        if (start && !s_setup) {
            setup_on();
            idle_since = esp_timer_get_time();
        }
        start = false;
        if (s_setup && phones()) {
            idle_since = now;
        }
        if (s_setup && s_have_saved && now - idle_since > (int64_t)SETUP_IDLE_S * 1000000) {
            setup_off();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t net_wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    settings_init();

    s_lock = xSemaphoreCreateMutex();
    settings_t set;
    settings_get(&set);
    s_have_saved = set.wifi_ssid[0] != '\0';
    snprintf(s_saved, sizeof(s_saved), "%s", set.wifi_ssid);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, NET_HOSTNAME);
    esp_netif_create_default_wifi_ap();   // 192.168.4.1, with a DHCP server for the phone

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // settings.h keeps the network, not the driver
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_event, NULL, NULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "N2K-CAN-%02X%02X", mac[4], mac[5]);

    wifi_config_t wc = { 0 };
    // A 32-character name fills ssid with no NUL, which the driver allows.
    memcpy(wc.sta.ssid, set.wifi_ssid, strnlen(set.wifi_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, set.wifi_pass, strnlen(set.wifi_pass, sizeof(wc.sta.password) - 1));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_down_since_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_wifi_start());

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(NET_HOSTNAME);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: %s.local", NET_HOSTNAME);
    }

    xTaskCreate(wifi_task, "wifi_setup", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "STA started%s%s", s_have_saved ? ", joining " : "; no network saved",
             s_have_saved ? s_saved : "");
    return ESP_OK;
}

bool net_wifi_connected(void)
{
    return s_connected;
}

int net_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

esp_err_t net_wifi_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;
    if (!s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        if (len) {
            buf[0] = '\0';
        }
        return ESP_FAIL;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

void net_wifi_setup_info(wifi_setup_t *out)
{
    *out = (wifi_setup_t){ .on = s_setup, .open = s_setup && s_setup_open,
                           .phones = phones(), .have_saved = s_have_saved };
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ap_ssid);
    snprintf(out->saved, sizeof(out->saved), "%s", s_saved);
}

int net_wifi_setup_networks(char names[][33], int max)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_net_count < max ? s_net_count : max;
    memcpy(names, s_nets, sizeof(s_nets[0]) * n);
    xSemaphoreGive(s_lock);
    return n;
}
