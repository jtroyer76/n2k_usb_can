#include "status_led.h"

#include "board.h"
#include "esp_timer.h"
#include "led_strip.h"

static led_strip_handle_t s_strip;

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    return led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

void status_led_update(led_state_t state, bool activity)
{
    if (!s_strip) {
        return;
    }
    // Two blink phases at 250 ms
    bool phase = (esp_timer_get_time() / 250000) & 1;

    switch (state) {
    case LED_NO_HOST:
        set_rgb(40, 0, 0);
        break;
    case LED_IDLE:
        set_rgb(0, 0, 40);
        break;
    case LED_BUS_OK:
        if (activity) {
            set_rgb(0, 120, 0);
        } else {
            set_rgb(0, 40, 0);
        }
        break;
    case LED_BUS_WARN:
        set_rgb(activity ? 120 : 40, activity ? 60 : 20, 0);
        break;
    case LED_BUS_OFF:
        set_rgb(phase ? 120 : 0, 0, 0);
        break;
    case LED_IDENTIFY:
        set_rgb(phase ? 120 : 0, 0, phase ? 120 : 0);
        break;
    }
}
