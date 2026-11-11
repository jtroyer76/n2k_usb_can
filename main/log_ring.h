// The last LOG_RING_LINES log lines, kept in RAM for the web console (GET /api/log).
// Captures everything that goes through ESP_LOGx: the lines still go to UART0 as before,
// and never into a USB endpoint. Lines are numbered from 1 so a client can ask for
// "everything after N".
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LOG_RING_LINES  160
#define LOG_RING_LEN    128

// Hooks ESP_LOG output. Call first thing in app_main.
void log_ring_init(void);

// Writes the GET /api/log body for lines after `since` into out (NUL-terminated):
//   {"lines":[[seq,ms,"I","text"],...],"next":S,"more":B,"lost":L,"reset":R}
// next:  pass as `since` next time.  more: more lines waiting, ask again now.
// lost:  lines after `since` already dropped.  reset: `since` was ahead (device restarted).
size_t log_ring_json(uint32_t since, char *out, size_t len);
