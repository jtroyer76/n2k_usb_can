#include "captive_dns.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "net_wifi.h"

static const char *TAG = "dns";
static volatile bool s_run;
static TaskHandle_t s_task;

// Turns a query into its answer in place: the question, then one A record (only for A
// questions; others get no records). Returns the length to send, 0 to ignore.
static int answer(uint8_t *m, int len, int max)
{
    if (len < 12 || (m[2] & 0x80) || (m[2] & 0x78) || m[4] || m[5] != 1) return 0;   // a standard query, one question
    int p = 12;
    while (p < len && m[p]) p += m[p] + 1;   // the name's labels
    if (p + 5 > len) return 0;
    p++;
    bool type_a = m[p] == 0 && m[p + 1] == 1;
    p += 4;   // type, class: the end of the question (anything after it, e.g. EDNS, is dropped)
    m[2] = 0x84 | (m[2] & 0x01);   // response, authoritative, keep "recursion desired"
    m[3] = 0x00;
    m[6] = 0, m[7] = type_a ? 1 : 0;
    m[8] = m[9] = m[10] = m[11] = 0;
    if (!type_a) return p;
    if (p + 16 > max) return 0;
    static const uint8_t rec[12] = { 0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4 };   // the name above, A, IN, 30 s
    memcpy(m + p, rec, sizeof(rec));
    uint32_t ip = inet_addr(SETUP_IP);
    memcpy(m + p + 12, &ip, 4);
    return p + 16;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct timeval tv = { .tv_sec = 1 };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "can't listen on port 53");
        if (sock >= 0) close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) continue;
        int out = answer(buf, n, sizeof(buf));
        if (out) sendto(sock, buf, out, 0, (struct sockaddr *)&from, from_len);
    }
    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(void)
{
    if (s_task) return;
    s_run = true;
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3, &s_task);
}

void captive_dns_stop(void)
{
    s_run = false;   // the task ends within a second
}
