// The gateway's web interface (port 80):
//
//   GET  /              status page + console (web/index.html); redirects to /setup on the hotspot
//   GET  /api/status    JSON: firmware, CAN, USB, WiFi, uptime, memory
//   GET  /api/log       JSON: console lines after ?since=N (log_ring.h)
//   POST /api/restart   restart the gateway
//   GET/POST /setup     settings + firmware upload (net_setup.c)
//   POST /update        firmware OTA (net_ota.c)
#pragma once

#include "esp_err.h"

esp_err_t net_web_init(void);
