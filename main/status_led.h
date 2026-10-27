// AtomS3 Lite RGB LED.

#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    LED_NO_HOST,        // powered, USB host not enumerated
    LED_IDLE,           // enumerated, CAN channel closed
    LED_BUS_OK,         // CAN running, error active
    LED_BUS_WARN,       // error warning / passive
    LED_BUS_OFF,
    LED_IDENTIFY,
} led_state_t;

esp_err_t status_led_init(void);

// Call periodically (~20 Hz). `activity` flashes the LED briefly.
void status_led_update(led_state_t state, bool activity);
