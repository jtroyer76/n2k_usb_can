// POST /update: firmware OTA, gated by the device password in the X-OTA-Key header. The new
// image boots pending-verify (ota_health.c). Used by curl and by the settings page's upload.
#pragma once

#include "esp_http_server.h"

void net_ota_register(httpd_handle_t server);
