// The setup page: WiFi network and device password.
//
//   GET  /setup   the page: from the setup hotspot (http://192.168.4.1/, phones open it by
//                 themselves), or from the boat network (http://n2k-can-gw.local/setup)
//   POST /setup   save and restart. Once a network has been saved, this needs the current
//                 device password; a first setup (nothing saved yet) doesn't.
//
// While the hotspot is on, / and any unknown address redirect to the page (the phone's
// captive-portal check).
#pragma once

#include "esp_http_server.h"

void net_setup_register(httpd_handle_t server);
