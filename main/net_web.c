#include "net_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "can_bus.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gs_usb_dev.h"
#include "log_ring.h"
#include "net_ota.h"
#include "net_setup.h"
#include "net_wifi.h"
#include "ota_health.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static char s_out[6144];   // one response at a time: the default httpd has one worker

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "restart";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_USB:       return "USB";
    default:                return "other";
    }
}

static const char *can_state_name(uint32_t st)
{
    switch (st) {
    case GS_CAN_STATE_ERROR_ACTIVE:  return "error-active";
    case GS_CAN_STATE_ERROR_WARNING: return "error-warning";
    case GS_CAN_STATE_ERROR_PASSIVE: return "error-passive";
    case GS_CAN_STATE_BUS_OFF:       return "bus-off";
    default:                         return "stopped";
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *body, size_t len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, len);
}

static esp_err_t page_get(httpd_req_t *req)
{
    wifi_setup_t ws;
    net_wifi_setup_info(&ws);
    if (ws.on) {   // on the setup hotspot: the phone wants the setup page
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

// SSIDs are the only free text here; drop anything that would need escaping.
static void json_safe(char *dst, size_t len, const char *src)
{
    size_t n = 0;
    for (; *src && n + 1 < len; src++) {
        if ((unsigned char)*src >= 0x20 && *src != '"' && *src != '\\') {
            dst[n++] = *src;
        }
    }
    dst[n] = '\0';
}

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    can_stats_t cs;
    can_bus_get_stats(&cs);
    gs_device_state_t st;
    can_bus_get_state(&st);
    bool running = can_bus_running();

    wifi_setup_t ws;
    net_wifi_setup_info(&ws);
    char ip[16] = "", ssid[33] = "";
    bool connected = net_wifi_connected();
    if (connected) {
        net_wifi_ip(ip, sizeof(ip));
        json_safe(ssid, sizeof(ssid), ws.saved);
    }
    char hs_ssid[33];
    json_safe(hs_ssid, sizeof(hs_ssid), ws.ssid);

    int n = snprintf(s_out, sizeof(s_out),
        "{\"version\":\"%s\",\"built\":\"%s %s\",\"partition\":\"%s\",\"pending_verify\":%s,"
        "\"uptime_ms\":%llu,\"reset\":\"%s\","
        "\"heap\":{\"free\":%lu,\"min\":%lu},"
        "\"usb\":{\"mounted\":%s},"
        "\"can\":{\"running\":%s,\"state\":\"%s\",\"bitrate\":%lu,\"listen_only\":%s,"
        "\"tec\":%lu,\"rec\":%lu,\"rx\":%lu,\"tx\":%lu,\"tx_failed\":%lu,\"rx_overflow\":%lu,"
        "\"bus_errors\":%lu,\"bus_off\":%lu},"
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\","
        "\"hotspot\":%s,\"hotspot_ssid\":\"%s\",\"hotspot_clients\":%d}}",
        app->version, app->date, app->time, run ? run->label : "?",
        ota_health_pending() ? "true" : "false",
        (unsigned long long)(esp_timer_get_time() / 1000), reset_reason(),
        (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
        gs_usb_dev_mounted() ? "true" : "false",
        running ? "true" : "false", running ? can_state_name(st.state) : "stopped",
        (unsigned long)can_bus_bitrate(), can_bus_listen_only() ? "true" : "false",
        (unsigned long)st.txerr, (unsigned long)st.rxerr,
        (unsigned long)cs.rx_frames, (unsigned long)cs.tx_frames, (unsigned long)cs.tx_failed,
        (unsigned long)cs.rx_overflows, (unsigned long)cs.bus_errors, (unsigned long)cs.bus_off_count,
        connected ? "true" : "false", ssid, connected ? net_wifi_rssi() : 0, ip,
        ws.on ? "true" : "false", hs_ssid, ws.phones);
    return send_json(req, s_out, n);
}

static esp_err_t log_get(httpd_req_t *req)
{
    uint32_t since = 0;
    char q[32], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
        since = strtoul(v, NULL, 10);
    }
    size_t n = log_ring_json(since, s_out, sizeof(s_out));
    return send_json(req, s_out, n);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));   // let the reply go out
    esp_restart();
}

static esp_err_t restart_post(httpd_req_t *req)
{
    ESP_LOGW(TAG, "restart requested from the web page");
    send_json(req, "{\"ok\":true}", 11);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t net_web_init(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    static const httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = page_get };
    static const httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get };
    static const httpd_uri_t log = { .uri = "/api/log", .method = HTTP_GET, .handler = log_get };
    static const httpd_uri_t restart = { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_post };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &log);
    httpd_register_uri_handler(server, &restart);
    net_ota_register(server);
    net_setup_register(server);
    ESP_LOGI(TAG, "web interface on port 80");
    return ESP_OK;
}
