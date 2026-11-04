// n2k_usb_can: USB-to-CAN adapter (gs_usb / candleLight protocol)
// for M5Stack AtomS3 Lite + ATOMIC CAN Base.

#include "can_bus.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gs_usb_dev.h"
#include "net_ota.h"
#include "net_wifi.h"
#include "ota_health.h"
#include "status_led.h"

static const char *TAG = "main";

// Self-heal watchdog. On a live bus the device stack once wedged after ~1 h
// (the DWC2 USB stack halted an endpoint and never recovered - CAN kept
// running, but can0 was gone on the host until a physical replug). Detect that
// and restart, so the host re-enumerates can0 in a few seconds instead of
// going dark for hours. Signature of a wedge: still mounted and CAN still
// receiving (rx climbing), yet no USB transfer has completed for a while.
#define USB_WEDGE_TIMEOUT_US  (5LL * 1000 * 1000)

static led_state_t current_led_state(void)
{
    if (gs_usb_dev_identify()) {
        return LED_IDENTIFY;
    }
    if (!gs_usb_dev_mounted()) {
        return LED_NO_HOST;
    }
    if (!can_bus_running()) {
        return LED_IDLE;
    }
    gs_device_state_t st;
    can_bus_get_state(&st);
    switch (st.state) {
    case GS_CAN_STATE_ERROR_ACTIVE:
        return LED_BUS_OK;
    case GS_CAN_STATE_BUS_OFF:
        return LED_BUS_OFF;
    default:
        return LED_BUS_WARN;
    }
}

static void usb_wedge_check(bool can_active)
{
    // Only meaningful while the host has the channel open (CAN running) and
    // frames are actually arriving - an idle bus looks the same as a wedge
    // otherwise. A healthy device completes thousands of transfers a second
    // on a live bus, so no progress for seconds with CAN active means wedged.
    if (!gs_usb_dev_mounted() || !can_bus_running() || !can_active) {
        return;
    }
    int64_t stale = esp_timer_get_time() - gs_usb_dev_last_progress_us();
    if (stale > USB_WEDGE_TIMEOUT_US) {
        ESP_LOGE(TAG, "USB wedged: no transfer for %lld ms with CAN active; restarting",
                 stale / 1000);
        vTaskDelay(pdMS_TO_TICKS(20));  // let the log line out
        esp_restart();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(can_bus_init());
    if (status_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "status LED unavailable");
    }
    ESP_ERROR_CHECK(gs_usb_dev_init());

    // Must run before WiFi/OTA: records whether this image is pending-verify.
    ota_health_init();
    // WiFi + the OTA/stats HTTP server are best-effort: a USB CAN adapter must
    // still work with no network, so their failure is non-fatal.
    if (net_wifi_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed; running USB-only");
    }
    if (net_ota_init() != ESP_OK) {
        ESP_LOGW(TAG, "HTTP/OTA init failed");
    }

    uint32_t last_count = 0;
    for (;;) {
        can_stats_t stats;
        can_bus_get_stats(&stats);
        uint32_t count = stats.rx_frames + stats.tx_frames;
        bool can_active = (count != last_count);
        status_led_update(current_led_state(), can_active);
        usb_wedge_check(can_active);
        ota_health_poll();
        last_count = count;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
