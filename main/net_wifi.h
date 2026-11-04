// WiFi STA: joins the boat network so firmware can be updated over the air.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t net_wifi_init(void);
bool net_wifi_connected(void);
int net_wifi_rssi(void);
esp_err_t net_wifi_ip(char *buf, size_t len);
