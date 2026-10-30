#include "usb_console.h"

#include <stdio.h>
#include <string.h>
#include "can_bus.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_pins.h"
#include "soc/usb_serial_jtag_reg.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"

static const char *TAG = "console";

// Line received by the TinyUSB task, handled by usb_console_poll().
static char s_line[32];
static size_t s_line_len;
static volatile bool s_line_ready;
static volatile bool s_want_bootloader;

// ---------------------------------------------------------------------------
// Reboot into the ROM bootloader
// ---------------------------------------------------------------------------

// Port of arduino-esp32's usb_persist_restart(RESTART_BOOTLOADER) for the S3
// (cores/esp32/esp32-hal-tinyusb.c). The OTG PHY selection lives in the RTC
// domain and survives a software reset, so hand the pads back to the
// USB-Serial/JTAG controller first; the ROM's download mode talks over that.
static void reboot_to_bootloader(void)
{
    ESP_LOGI(TAG, "rebooting into bootloader");
    vTaskDelay(pdMS_TO_TICKS(50));      // let the log line out

    // Detach stdout first: a log line from another task must not reach a
    // torn-down CDC driver.
    tinyusb_console_deinit(TINYUSB_CDC_ACM_0);
    tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    tinyusb_driver_uninstall();

    // Route the internal PHY to USB-Serial/JTAG.
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

    // Hold D+/D- low so the host sees a disconnect and re-enumerates.
    gpio_set_direction(USBPHY_DM_NUM, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(USBPHY_DP_NUM, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(USBPHY_DM_NUM, 0);
    gpio_set_level(USBPHY_DP_NUM, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "unknown";
    }
}

static void print_stats(void)
{
    can_stats_t s;
    can_bus_get_stats(&s);
    gs_device_state_t st;
    can_bus_get_state(&st);
    static const char *const states[] = { "error-active", "error-warning", "error-passive", "bus-off", "stopped", "sleeping" };
    const char *state = st.state < sizeof(states) / sizeof(states[0]) ? states[st.state] : "?";

    printf("uptime %llu s, last reset: %s\n", esp_timer_get_time() / 1000000ULL,
           reset_reason_name(esp_reset_reason()));
    printf("can %s, state %s, tx_err %lu, rx_err %lu\n",
           can_bus_running() ? "up" : "down", state, st.txerr, st.rxerr);
    printf("rx %lu, tx %lu, tx_failed %lu, rx_overflows %lu, bus_errors %lu, bus_off %lu\n",
           s.rx_frames, s.tx_frames, s.tx_failed, s.rx_overflows, s.bus_errors, s.bus_off_count);
}

static void run_command(const char *cmd)
{
    if (strcmp(cmd, "stats") == 0) {
        print_stats();
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("rebooting\n");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    } else if (strcmp(cmd, "bootloader") == 0) {
        reboot_to_bootloader();
    } else if (cmd[0] != '\0') {
        printf("commands: stats, reboot, bootloader\n");
    }
}

// ---------------------------------------------------------------------------
// CDC callbacks (TinyUSB task)
// ---------------------------------------------------------------------------

static void on_rx(int itf, cdcacm_event_t *event)
{
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t n = 0;
    if (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < n && !s_line_ready; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            s_line[s_line_len] = '\0';
            s_line_ready = true;
        } else if (s_line_len < sizeof(s_line) - 1) {
            s_line[s_line_len++] = c;
        }
    }
}

static void on_line_coding(int itf, cdcacm_event_t *event)
{
    // 1200-baud touch, as Arduino tools and esptool-style flashers use.
    if (event->line_coding_changed_data.p_line_coding->bit_rate == 1200) {
        s_want_bootloader = true;
    }
}

// ---------------------------------------------------------------------------

esp_err_t usb_console_init(void)
{
    const tinyusb_config_cdcacm_t acm = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = on_rx,
        .callback_line_coding_changed = on_line_coding,
    };
    esp_err_t err = tinyusb_cdcacm_init(&acm);
    if (err != ESP_OK) {
        return err;
    }
    return tinyusb_console_init(TINYUSB_CDC_ACM_0);
}

void usb_console_poll(void)
{
    if (s_want_bootloader) {
        reboot_to_bootloader();
    }
    if (s_line_ready) {
        run_command(s_line);
        s_line_len = 0;
        s_line_ready = false;
    }
}
