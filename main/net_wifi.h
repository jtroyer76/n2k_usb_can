// WiFi: station mode with automatic reconnect, mDNS (n2k-can-gw.local), and the setup hotspot,
// where a phone joins the gateway to choose its WiFi network (net_setup.c). The network comes
// from settings.h.
//
// The hotspot (N2K-CAN-XXXX, at SETUP_IP) comes on by itself:
//  - when no network is saved: open, so a new gateway can be set up from a phone;
//  - when the saved network hasn't been joined for SETUP_AFTER_S: protected, with the device
//    password as its WiFi password. It goes off again after SETUP_IDLE_S with no phone on it,
//    and the saved network is tried again.
// While it's on, the saved network isn't tried (trying it scans every channel, which drops
// the phone).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define NET_HOSTNAME    "n2k-can-gw"
#define SETUP_AFTER_S   300
#define SETUP_IDLE_S    300
#define SETUP_IP        "192.168.4.1"
#define SETUP_MAX_NETS  16

// Starts NVS, loads the settings, and brings WiFi up.
esp_err_t net_wifi_init(void);
bool net_wifi_connected(void);
int net_wifi_rssi(void);
esp_err_t net_wifi_ip(char *buf, size_t len);

typedef struct {
    bool on;             // hotspot on
    bool open;           // no WiFi password on it (first setup)
    char ssid[33];       // the hotspot's name
    int phones;          // joined now
    bool have_saved;     // a saved network to go back to
    char saved[33];
} wifi_setup_t;
void net_wifi_setup_info(wifi_setup_t *out);

// Networks found as the hotspot came on, strongest first, no repeats. Returns how many.
int net_wifi_setup_networks(char names[][33], int max);
