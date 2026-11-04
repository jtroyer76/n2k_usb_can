// OTA rollback health-check. A freshly-OTA'd image boots pending-verify and
// is reverted by the bootloader on the next restart unless it proves the full
// data path works and marks itself valid. See ota_health.c.
#pragma once

#include <stdbool.h>

// Call once at startup (reads whether the running image is pending-verify).
void ota_health_init(void);

// Call periodically from the main loop. Marks the image valid once it has run
// healthy for long enough, or forces a rollback if it never does.
void ota_health_poll(void);

// True while the running image is still on probation (not yet marked valid).
bool ota_health_pending(void);
