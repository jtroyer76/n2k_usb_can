// USB side of the gateway: a gs_usb (candleLight-compatible) device.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t gs_usb_dev_init(void);

bool gs_usb_dev_mounted(void);

// True while the host has asked the LED to identify the device.
bool gs_usb_dev_identify(void);

// Timestamp (us, esp_timer) of the last completed USB transfer in either
// direction. Used by the app_main self-heal watchdog to detect a wedged
// USB stack while CAN traffic is still flowing.
int64_t gs_usb_dev_last_progress_us(void);
