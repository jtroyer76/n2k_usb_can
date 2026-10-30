#include "gs_usb_dev.h"

#include <stdio.h>
#include <string.h>
#include "can_bus.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gs_usb_proto.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "gs_usb";

// candleLight's default VID:PID (openmoko-assigned to Geschwister Schneider
// USB2CAN). Matches the gs_usb kernel driver on every kernel version.
#define USB_VID     0x1D50
#define USB_PID     0x606F

#define FW_VERSION  2       // gs_usb requires sw_version > 1 for IDENTIFY
#define HW_VERSION  1

// gs_usb: interface 0 (the kernel binds only interface 0), bulk IN 0x81 /
// OUT 0x02 as candleLight uses - older kernels hardcode these addresses.
#define ITF_NUM_GS_USB  0
#define EP_OUT          0x02
#define EP_IN           0x81
#define EP_SIZE         64

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_COUNT,
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_GS_USB, STRID_INTERFACE, EP_OUT, EP_IN, EP_SIZE),
};

static char s_serial[13];
static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID] = (const char[]){ 0x09, 0x04 },
    [STRID_MANUFACTURER] = "n2k_usb_can",
    [STRID_PRODUCT] = "AtomS3 CAN gateway (gs_usb)",
    [STRID_SERIAL] = s_serial,
    [STRID_INTERFACE] = "gs_usb",
};

static SemaphoreHandle_t s_in_done;
static uint8_t s_ctrl_buf[64];
static can_timing_t s_timing;
static bool s_timing_set;
static volatile bool s_identify;

// ---------------------------------------------------------------------------
// Control requests
// ---------------------------------------------------------------------------

static bool handle_out_data(const tusb_control_request_t *req)
{
    switch (req->bRequest) {
    case GS_USB_BREQ_HOST_FORMAT:
        // candleLight-compatible devices always use little endian.
        return true;

    case GS_USB_BREQ_BITTIMING: {
        gs_device_bittiming_t bt;
        memcpy(&bt, s_ctrl_buf, sizeof(bt));
        can_timing_t t = {
            .brp = bt.brp,
            .tseg1 = bt.prop_seg + bt.phase_seg1,
            .tseg2 = bt.phase_seg2,
            .sjw = bt.sjw,
        };
        if (!can_bus_timing_valid(&t)) {
            ESP_LOGW(TAG, "rejected bittiming brp=%lu tseg1=%lu tseg2=%lu sjw=%lu",
                     t.brp, t.tseg1, t.tseg2, t.sjw);
            return false;
        }
        s_timing = t;
        s_timing_set = true;
        return true;
    }

    case GS_USB_BREQ_MODE: {
        gs_device_mode_t m;
        memcpy(&m, s_ctrl_buf, sizeof(m));
        if (m.mode == GS_CAN_MODE_RESET) {
            can_bus_stop();
            return true;
        }
        if (m.mode != GS_CAN_MODE_START || !s_timing_set) {
            return false;
        }
        can_mode_t mode = {
            .listen_only = m.flags & GS_CAN_MODE_LISTEN_ONLY,
            .loopback = m.flags & GS_CAN_MODE_LOOP_BACK,
            .one_shot = m.flags & GS_CAN_MODE_ONE_SHOT,
            .triple_sample = m.flags & GS_CAN_MODE_TRIPLE_SAMPLE,
            .berr_reporting = m.flags & GS_CAN_MODE_BERR_REPORTING,
        };
        return can_bus_start(&s_timing, &mode) == ESP_OK;
    }

    case GS_USB_BREQ_IDENTIFY: {
        gs_identify_mode_t id;
        memcpy(&id, s_ctrl_buf, sizeof(id));
        s_identify = id.mode != 0;
        return true;
    }

    case GS_USB_BREQ_BERR:
        return true;

    default:
        return false;
    }
}

static bool handle_setup(uint8_t rhport, const tusb_control_request_t *req)
{
    static union {
        gs_device_config_t config;
        gs_device_bt_const_t bt_const;
        gs_device_state_t state;
        uint32_t timestamp;
    } in;

    switch (req->bRequest) {
    case GS_USB_BREQ_HOST_FORMAT:
    case GS_USB_BREQ_BITTIMING:
    case GS_USB_BREQ_MODE:
    case GS_USB_BREQ_IDENTIFY:
    case GS_USB_BREQ_BERR:
        if (req->wLength > sizeof(s_ctrl_buf)) {
            return false;
        }
        return tud_control_xfer(rhport, req, s_ctrl_buf, req->wLength);

    case GS_USB_BREQ_DEVICE_CONFIG:
        memset(&in.config, 0, sizeof(in.config));
        in.config.icount = 0;
        in.config.sw_version = FW_VERSION;
        in.config.hw_version = HW_VERSION;
        return tud_control_xfer(rhport, req, &in.config, TU_MIN(req->wLength, sizeof(in.config)));

    case GS_USB_BREQ_BT_CONST:
        in.bt_const = (gs_device_bt_const_t) {
            .feature = GS_CAN_FEATURE_LISTEN_ONLY | GS_CAN_FEATURE_LOOP_BACK |
                       GS_CAN_FEATURE_TRIPLE_SAMPLE | GS_CAN_FEATURE_ONE_SHOT |
                       GS_CAN_FEATURE_IDENTIFY | GS_CAN_FEATURE_BERR_REPORTING |
                       GS_CAN_FEATURE_GET_STATE,
            .fclk_can = CAN_FCLK_HZ,
            .tseg1_min = CAN_TSEG1_MIN,
            .tseg1_max = CAN_TSEG1_MAX,
            .tseg2_min = CAN_TSEG2_MIN,
            .tseg2_max = CAN_TSEG2_MAX,
            .sjw_max = CAN_SJW_MAX,
            .brp_min = CAN_BRP_MIN,
            .brp_max = CAN_BRP_MAX,
            .brp_inc = CAN_BRP_INC,
        };
        return tud_control_xfer(rhport, req, &in.bt_const, TU_MIN(req->wLength, sizeof(in.bt_const)));

    case GS_USB_BREQ_GET_STATE:
        can_bus_get_state(&in.state);
        return tud_control_xfer(rhport, req, &in.state, TU_MIN(req->wLength, sizeof(in.state)));

    case GS_USB_BREQ_TIMESTAMP:
        in.timestamp = (uint32_t)esp_timer_get_time();
        return tud_control_xfer(rhport, req, &in.timestamp, TU_MIN(req->wLength, sizeof(in.timestamp)));

    default:
        return false;
    }
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        req->wIndex != ITF_NUM_GS_USB) {
        return false;
    }
    // wValue carries the channel; we have exactly one.
    if (req->wValue != 0 && req->bRequest != GS_USB_BREQ_HOST_FORMAT &&
        req->bRequest != GS_USB_BREQ_DEVICE_CONFIG) {
        return false;
    }

    switch (stage) {
    case CONTROL_STAGE_SETUP:
        return handle_setup(rhport, req);
    case CONTROL_STAGE_DATA:
        if (req->bmRequestType_bit.direction == TUSB_DIR_OUT) {
            return handle_out_data(req);
        }
        return true;
    default:
        return true;
    }
}

// ---------------------------------------------------------------------------
// Bulk data
// ---------------------------------------------------------------------------

// TinyUSB defaults CFG_TUD_API_V0_19_COMPAT on, which keeps a 16-bit length.
#if CFG_TUD_API_V0_19_COMPAT
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
#else
void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint32_t bufsize)
#endif
{
    if (!buffer || bufsize < offsetof(gs_host_frame_t, data)) {
        return;
    }
    gs_host_frame_t f = { 0 };
    memcpy(&f, buffer, TU_MIN(bufsize, sizeof(f)));
    if (f.channel != 0) {
        return;
    }
    can_bus_send(&f);
}

void tud_vendor_tx_cb(uint8_t idx, uint32_t sent_bytes)
{
    xSemaphoreGive(s_in_done);
}

// At least one tick: pdMS_TO_TICKS(2) is 0 at a 100 Hz tick, which turns
// these waits into a busy loop that starves the main task.
#define SHORT_WAIT (pdMS_TO_TICKS(2) > 0 ? pdMS_TO_TICKS(2) : 1)

static bool send_to_host(const gs_host_frame_t *f)
{
    // FIFO-less vendor mode: one frame per bulk transfer, wait for the
    // endpoint to be free between frames.
    for (int tries = 0; tries < 50; tries++) {
        if (!tud_vendor_mounted()) {
            return false;
        }
        if (tud_vendor_write_available() > 0) {
            if (tud_vendor_write(f, sizeof(*f)) == sizeof(*f)) {
                return true;
            }
        }
        xSemaphoreTake(s_in_done, SHORT_WAIT);
    }
    ESP_LOGW(TAG, "IN endpoint stuck, frame dropped");
    return false;
}

static void usb_in_task(void *arg)
{
    QueueHandle_t echo_q = can_bus_echo_queue();
    QueueHandle_t rx_q = can_bus_rx_queue();
    gs_host_frame_t f;

    for (;;) {
        // Echoes first: they gate the host's TX queue.
        if (xQueueReceive(echo_q, &f, 0) == pdTRUE ||
            xQueueReceive(rx_q, &f, SHORT_WAIT) == pdTRUE) {
            if (can_bus_running()) {
                send_to_host(&f);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_DETACHED) {
        s_identify = false;
        can_bus_stop();
    }
}

esp_err_t gs_usb_dev_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_in_done = xSemaphoreCreateBinary();
    if (!s_in_done) {
        return ESP_ERR_NO_MEM;
    }

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    cfg.descriptor.device = &s_device_desc;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = STRID_COUNT;
    cfg.descriptor.full_speed_config = s_config_desc;

    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(usb_in_task, "usb_in", 4096, NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "gs_usb device %04X:%04X serial %s", USB_VID, USB_PID, s_serial);
    return ESP_OK;
}

bool gs_usb_dev_mounted(void)
{
    return tud_mounted();
}

bool gs_usb_dev_identify(void)
{
    return s_identify;
}
