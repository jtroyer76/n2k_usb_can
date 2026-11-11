#include "log_ring.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    uint32_t seq;
    uint32_t ms;       // uptime when logged
    char level;        // 'E', 'W', 'I', 'D', 'V'
    char text[LOG_RING_LEN];
} line_t;

static line_t s_lines[LOG_RING_LINES];
static uint32_t s_last;   // seq of the newest line; 0 = none
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

// ESP_LOG lines look like "\033[0;32mI (1234) tag: text\033[0m\n". Keep the level letter and
// "tag: text"; drop colour codes, the timestamp (we keep our own) and the newline.
// Cleans `clean` in place (output never runs ahead of input), then stores it.
static void push(char *clean)
{
    size_t n = 0;
    for (const char *p = clean; *p; p++) {
        if (*p == '\033') {   // skip "\033[...m"
            while (*p && *p != 'm') {
                p++;
            }
            if (!*p) {
                break;
            }
            continue;
        }
        if (*p == '\r' || *p == '\n') {
            continue;
        }
        clean[n++] = *p;
    }
    clean[n] = '\0';
    if (!n) {
        return;
    }

    char level = 'I';
    const char *text = clean;
    if (strchr("EWIDV", clean[0]) && clean[1] == ' ' && clean[2] == '(') {
        level = clean[0];
        const char *close = strchr(clean, ')');
        if (close) {
            text = close + 1;
            while (*text == ' ') {
                text++;
            }
        }
    }

    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_mux);
    line_t *l = &s_lines[s_last % LOG_RING_LINES];
    s_last++;
    l->seq = s_last;
    l->ms = ms;
    l->level = level;
    strlcpy(l->text, text, sizeof(l->text));
    portEXIT_CRITICAL(&s_mux);
}

// Some code (the WiFi driver) writes a line in pieces: "I (123) wifi:" then "text\n". Pieces
// collect here until one ends the line.
static char s_pending[LOG_RING_LEN + 32];

static int hook(const char *fmt, va_list args)
{
    char buf[sizeof(s_pending)];
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    // A piece too long for buf lost its newline: treat it as the end of the line.
    bool line_ends = len <= 0 || len >= (int)sizeof(buf) - 1 || buf[len - 1] == '\n';

    portENTER_CRITICAL(&s_mux);
    strlcat(s_pending, buf, sizeof(s_pending));
    if (line_ends) {
        memcpy(buf, s_pending, sizeof(buf));
        s_pending[0] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
    if (line_ends) {
        push(buf);
    }
    return s_prev ? s_prev(fmt, args) : 0;   // still to UART0
}

void log_ring_init(void)
{
    s_prev = esp_log_set_vprintf(hook);
}

// Appends s as a JSON string. False if it didn't fit.
static bool put_str(char *out, size_t len, size_t *n, const char *s)
{
    size_t i = *n;
    if (i + 2 >= len) {
        return false;
    }
    out[i++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[7];
        const char *e = NULL;
        if (c == '"') {
            e = "\\\"";
        } else if (c == '\\') {
            e = "\\\\";
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            e = esc;
        }
        size_t need = e ? strlen(e) : 1;
        if (i + need + 2 >= len) {
            return false;
        }
        if (e) {
            memcpy(out + i, e, need);
            i += need;
        } else {
            out[i++] = (char)c;
        }
    }
    out[i++] = '"';
    out[i] = '\0';
    *n = i;
    return true;
}

static bool put_raw(char *out, size_t len, size_t *n, const char *s)
{
    size_t k = strlen(s);
    if (*n + k + 1 >= len) {
        return false;
    }
    memcpy(out + *n, s, k + 1);
    *n += k;
    return true;
}

#define TAIL_RESERVE 80   // ],"next":<u32>,"more":false,"lost":<u32>,"reset":false}

size_t log_ring_json(uint32_t since, char *out, size_t len)
{
    uint32_t last;
    portENTER_CRITICAL(&s_mux);
    last = s_last;
    portEXIT_CRITICAL(&s_mux);
    uint32_t oldest = last >= LOG_RING_LINES ? last - LOG_RING_LINES + 1 : 1;

    bool reset = since > last;
    if (reset) {
        since = 0;
    }
    uint32_t start = since + 1 < oldest ? oldest : since + 1;
    uint32_t lost = start - 1 - since;
    uint32_t next = start - 1;
    bool more = false;

    size_t n = 0;
    out[0] = '\0';
    put_raw(out, len, &n, "{\"lines\":[");
    size_t body_len = len > TAIL_RESERVE ? len - TAIL_RESERVE : 0;
    int written = 0;
    for (uint32_t seq = start; seq <= last; seq++) {
        line_t l;
        portENTER_CRITICAL(&s_mux);
        l = s_lines[(seq - 1) % LOG_RING_LINES];
        portEXIT_CRITICAL(&s_mux);
        if (l.seq != seq) {   // overwritten since we looked
            more = true;
            break;
        }
        size_t mark = n;
        char head[48], level[2] = { l.level, 0 };
        snprintf(head, sizeof(head), "%s[%lu,%lu,", written ? "," : "",
                 (unsigned long)l.seq, (unsigned long)l.ms);
        bool ok = put_raw(out, body_len, &n, head) && put_str(out, body_len, &n, level) &&
                  put_raw(out, body_len, &n, ",") && put_str(out, body_len, &n, l.text) &&
                  put_raw(out, body_len, &n, "]");
        if (!ok) {
            n = mark;
            out[n] = '\0';
            more = true;
            break;
        }
        next = seq;
        written++;
    }
    char tail[TAIL_RESERVE];
    snprintf(tail, sizeof(tail), "],\"next\":%lu,\"more\":%s,\"lost\":%lu,\"reset\":%s}",
             (unsigned long)next, more ? "true" : "false", (unsigned long)lost,
             reset ? "true" : "false");
    put_raw(out, len, &n, tail);
    return n;
}
