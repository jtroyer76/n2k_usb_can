// HTTP server: GET / (plain-text stats) and POST /update (firmware OTA,
// gated by the X-OTA-Key header). Starts after WiFi.
#pragma once

#include "esp_err.h"

esp_err_t net_ota_init(void);
