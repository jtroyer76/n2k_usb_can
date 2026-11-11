#include "net_ota.h"

#include <stdio.h>
#include <string.h>
#include "can_bus.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gs_usb_dev.h"
#include "net_wifi.h"
#include "net_setup.h"
#include "ota_health.h"
#include "settings.h"

static const char *TAG = "net_ota";

static httpd_handle_t s_server;
static char s_ota_buf[2048];   // single handler at a time (default httpd worker)

static esp_err_t stats_get(httpd_req_t *req)
{
    wifi_setup_t ws;
    net_wifi_setup_info(&ws);
    if (ws.on) {   // on the setup hotspot: the phone wants the setup page
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        return httpd_resp_send(req, NULL, 0);
    }
    can_stats_t s;
    can_bus_get_stats(&s);
    gs_device_state_t cs;
    can_bus_get_state(&cs);
    char ip[16];
    net_wifi_ip(ip, sizeof(ip));
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();

    char body[700];
    int n = snprintf(body, sizeof(body),
        "n2k_usb_can gateway\n"
        "fw %s  built %s %s\n"
        "partition %s  pending-verify %s\n"
        "uptime %llu s\n"
        "wifi %s  rssi %d dBm  ip %s\n"
        "usb mounted %s\n"
        "can %s  state %lu  tx_err %lu  rx_err %lu\n"
        "rx %lu  tx %lu  tx_failed %lu  rx_overflow %lu  bus_err %lu  bus_off %lu\n"
        "settings: /setup\n",
        app->version, app->date, app->time,
        run->label, ota_health_pending() ? "yes" : "no",
        (unsigned long long)(esp_timer_get_time() / 1000000),
        net_wifi_connected() ? "up" : "down", net_wifi_rssi(), ip,
        gs_usb_dev_mounted() ? "yes" : "no",
        can_bus_running() ? "up" : "down", (unsigned long)cs.state,
        (unsigned long)cs.txerr, (unsigned long)cs.rxerr,
        (unsigned long)s.rx_frames, (unsigned long)s.tx_frames, (unsigned long)s.tx_failed,
        (unsigned long)s.rx_overflows, (unsigned long)s.bus_errors, (unsigned long)s.bus_off_count);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static bool authed(httpd_req_t *req)
{
    // The device password (settings.h), sent as X-OTA-Key.
    char key[SETTINGS_PW_LEN + 1];
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Key", key, sizeof(key)) != ESP_OK) {
        return false;
    }
    return settings_password_ok(key);
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (!authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad or missing X-OTA-Key (the device password)");
        return ESP_FAIL;
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(next, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA receiving %d bytes into %s", req->content_len, next->label);

    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining < (int)sizeof(s_ota_buf) ? remaining : (int)sizeof(s_ota_buf);
        int r = httpd_req_recv(req, s_ota_buf, want);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota, s_ota_buf, r) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            return ESP_FAIL;
        }
        remaining -= r;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            err == ESP_ERR_OTA_VALIDATE_FAILED ? "image invalid" : esp_err_to_name(err));
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "OTA written to %s; rebooting (pending-verify, rolls back unless healthy)",
             next->label);
    httpd_resp_sendstr(req, "ok: rebooting into new image (pending verify)\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t net_ota_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    httpd_uri_t stats = { .uri = "/", .method = HTTP_GET, .handler = stats_get };
    httpd_uri_t upd = { .uri = "/update", .method = HTTP_POST, .handler = update_post };
    httpd_register_uri_handler(s_server, &stats);
    httpd_register_uri_handler(s_server, &upd);
    net_setup_register(s_server);
    ESP_LOGI(TAG, "http up: GET / (stats), /setup (settings), POST /update (OTA)");
    return ESP_OK;
}
