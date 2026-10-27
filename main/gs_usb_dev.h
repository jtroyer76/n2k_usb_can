// USB side of the gateway: a gs_usb (candleLight-compatible) device.

#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t gs_usb_dev_init(void);

bool gs_usb_dev_mounted(void);

// True while the host has asked the LED to identify the device.
bool gs_usb_dev_identify(void);
