#include "net_ota.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

static const char *TAG = "net_ota";

static char s_ota_buf[2048];   // single handler at a time (default httpd worker)

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

void net_ota_register(httpd_handle_t server)
{
    static const httpd_uri_t upd = { .uri = "/update", .method = HTTP_POST, .handler = update_post };
    httpd_register_uri_handler(server, &upd);
}
