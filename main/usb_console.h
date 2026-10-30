#pragma once

#include "esp_err.h"

// CDC-ACM console on USB interfaces 1-2, beside gs_usb on interface 0.
// Carries the log output and a few line commands (stats, reboot,
// bootloader). Opening the port at 1200 baud reboots into the ROM
// bootloader so the board can be reflashed without the reset button.

// Call after tinyusb_driver_install().
esp_err_t usb_console_init(void);

// Call from the main loop. Runs commands and the bootloader reboot outside
// the TinyUSB task, which they would otherwise have to tear down from inside.
void usb_console_poll(void);
