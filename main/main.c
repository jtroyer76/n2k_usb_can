// n2k_usb_can: USB-to-CAN adapter (gs_usb / candleLight protocol)
// for M5Stack AtomS3 Lite + ATOMIC CAN Base.

#include "can_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gs_usb_dev.h"
#include "status_led.h"
#include "usb_console.h"

static const char *TAG = "main";

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

void app_main(void)
{
    ESP_ERROR_CHECK(can_bus_init());
    if (status_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "status LED unavailable");
    }
    ESP_ERROR_CHECK(gs_usb_dev_init());

    uint32_t last_count = 0;
    for (;;) {
        can_stats_t stats;
        can_bus_get_stats(&stats);
        uint32_t count = stats.rx_frames + stats.tx_frames;
        status_led_update(current_led_state(), count != last_count);
        usb_console_poll();
        last_count = count;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
