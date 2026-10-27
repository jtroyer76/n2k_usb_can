// Pin map for M5Stack AtomS3 Lite + ATOMIC CAN Base.
//
// CAN pins come from M5Stack's own AtomS3 example
// (M5AtomS3 examples/AtomBase/AtomCan/AtomCan.ino: TX_PIN 5, RX_PIN 6).
// Some M5Stack prose docs list them reversed; the example code is correct.

#pragma once

#include "driver/gpio.h"

#define BOARD_CAN_TX_GPIO   GPIO_NUM_5
#define BOARD_CAN_RX_GPIO   GPIO_NUM_6

// WS2812 RGB LED (M5Unified board table for board_M5AtomS3Lite)
#define BOARD_LED_GPIO      GPIO_NUM_35

// Front button, active low (M5Unified)
#define BOARD_BUTTON_GPIO   GPIO_NUM_41
