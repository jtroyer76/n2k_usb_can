#include "can_bus.h"

#include <string.h>
#include "board.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "can_bus";

#define RX_QUEUE_LEN    64
#define TX_QUEUE_DEPTH  (GS_MAX_TX_URBS + 2)

// One slot per host echo_id. The TWAI driver keeps a pointer to the frame
// until on_tx_done, so frame and data must live here, not on a stack.
typedef struct {
    bool in_use;
    twai_frame_t frame;
    uint8_t data[8];
    gs_host_frame_t echo;
} tx_slot_t;

static twai_node_handle_t s_node;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_rx_queue;
static QueueHandle_t s_echo_queue;
static TaskHandle_t s_recover_task;
static tx_slot_t s_slots[GS_MAX_TX_URBS];
static can_mode_t s_mode;
static volatile bool s_rx_overflow;
static volatile twai_error_state_t s_state = TWAI_ERROR_ACTIVE;
static can_stats_t s_stats;

static void fill_error_frame(gs_host_frame_t *f, uint32_t err_id)
{
    memset(f, 0, sizeof(*f));
    f->echo_id = GS_HOST_FRAME_ECHO_ID_RX;
    f->can_id = CAN_ERR_FLAG | err_id;
    f->can_dlc = CAN_ERR_DLC;
}

static bool push_rx_from_isr(gs_host_frame_t *f)
{
    BaseType_t woken = pdFALSE;
    if (s_rx_overflow) {
        f->flags |= GS_CAN_FLAG_OVERFLOW;
    }
    if (xQueueSendFromISR(s_rx_queue, f, &woken) == pdTRUE) {
        s_rx_overflow = false;
    } else {
        s_rx_overflow = true;
        s_stats.rx_overflows++;
    }
    return woken == pdTRUE;
}

static bool on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *ctx)
{
    uint8_t data[8];
    twai_frame_t rx = { .buffer = data, .buffer_len = sizeof(data) };
    if (twai_node_receive_from_isr(handle, &rx) != ESP_OK) {
        return false;
    }

    gs_host_frame_t f = { .echo_id = GS_HOST_FRAME_ECHO_ID_RX };
    f.can_id = rx.header.ide ? (rx.header.id & CAN_EFF_MASK) | CAN_EFF_FLAG
                             : (rx.header.id & CAN_SFF_MASK);
    if (rx.header.rtr) {
        f.can_id |= CAN_RTR_FLAG;
    }
    f.can_dlc = rx.header.dlc > 8 ? 8 : rx.header.dlc;
    if (!rx.header.rtr) {
        memcpy(f.data, data, f.can_dlc);
    }
    s_stats.rx_frames++;
    return push_rx_from_isr(&f);
}

static bool on_tx_done(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *ctx)
{
    tx_slot_t *slot = __containerof(edata->done_tx_frame, tx_slot_t, frame);
    if (slot < &s_slots[0] || slot >= &s_slots[GS_MAX_TX_URBS]) {
        return false;
    }
    if (edata->is_tx_success) {
        s_stats.tx_frames++;
    } else {
        s_stats.tx_failed++;
    }
    // The Linux driver has no "TX failed" echo; the slot must be released
    // either way or its TX queue stalls. Failures surface as error frames.
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_echo_queue, &slot->echo, &woken);
    slot->in_use = false;
    return woken == pdTRUE;
}

static uint8_t crtl_bits_for(twai_error_state_t st)
{
    switch (st) {
    case TWAI_ERROR_WARNING:
        return CAN_ERR_CRTL_TX_WARNING | CAN_ERR_CRTL_RX_WARNING;
    case TWAI_ERROR_PASSIVE:
        return CAN_ERR_CRTL_TX_PASSIVE | CAN_ERR_CRTL_RX_PASSIVE;
    default:
        return CAN_ERR_CRTL_ACTIVE;
    }
}

static bool on_state_change(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *ctx)
{
    s_state = edata->new_sta;
    gs_host_frame_t f;
    bool woken = false;

    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_stats.bus_off_count++;
        fill_error_frame(&f, CAN_ERR_BUSOFF);
        woken |= push_rx_from_isr(&f);
        // The gs_usb driver cannot restart a channel, so recover here.
        BaseType_t w = pdFALSE;
        vTaskNotifyGiveFromISR(s_recover_task, &w);
        woken |= (w == pdTRUE);
    } else if (edata->old_sta == TWAI_ERROR_BUS_OFF) {
        fill_error_frame(&f, CAN_ERR_RESTARTED);
        woken |= push_rx_from_isr(&f);
    } else {
        fill_error_frame(&f, CAN_ERR_CRTL);
        f.data[1] = crtl_bits_for(edata->new_sta);
        woken |= push_rx_from_isr(&f);
    }
    return woken;
}

static bool on_error(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *ctx)
{
    s_stats.bus_errors++;
    if (!s_mode.berr_reporting) {
        return false;
    }
    gs_host_frame_t f;
    uint32_t id = CAN_ERR_BUSERROR;
    if (edata->err_flags.arb_lost) {
        id |= CAN_ERR_LOSTARB;
    }
    if (edata->err_flags.ack_err) {
        id |= CAN_ERR_ACK;
    }
    if (edata->err_flags.bit_err || edata->err_flags.form_err || edata->err_flags.stuff_err) {
        id |= CAN_ERR_PROT;
    }
    fill_error_frame(&f, id);
    if (edata->err_flags.bit_err) {
        f.data[2] |= CAN_ERR_PROT_BIT;
    }
    if (edata->err_flags.form_err) {
        f.data[2] |= CAN_ERR_PROT_FORM;
    }
    if (edata->err_flags.stuff_err) {
        f.data[2] |= CAN_ERR_PROT_STUFF;
    }
    return push_rx_from_isr(&f);
}

static void recover_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Let the bus settle briefly before asking the controller to rejoin.
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_node && s_state == TWAI_ERROR_BUS_OFF) {
            esp_err_t err = twai_node_recover(s_node);
            ESP_LOGW(TAG, "bus-off, recovery started: %s", esp_err_to_name(err));
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t can_bus_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(gs_host_frame_t));
    s_echo_queue = xQueueCreate(GS_MAX_TX_URBS, sizeof(gs_host_frame_t));
    if (!s_lock || !s_rx_queue || !s_echo_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(recover_task, "can_recover", 3072, NULL, 6, &s_recover_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

QueueHandle_t can_bus_rx_queue(void)
{
    return s_rx_queue;
}

QueueHandle_t can_bus_echo_queue(void)
{
    return s_echo_queue;
}

bool can_bus_timing_valid(const can_timing_t *t)
{
    return t->brp >= CAN_BRP_MIN && t->brp <= CAN_BRP_MAX && (t->brp % CAN_BRP_INC) == 0 &&
           t->tseg1 >= CAN_TSEG1_MIN && t->tseg1 <= CAN_TSEG1_MAX &&
           t->tseg2 >= CAN_TSEG2_MIN && t->tseg2 <= CAN_TSEG2_MAX &&
           t->sjw >= 1 && t->sjw <= CAN_SJW_MAX;
}

static void stop_locked(void)
{
    if (!s_node) {
        return;
    }
    twai_node_disable(s_node);
    twai_node_delete(s_node);
    s_node = NULL;
    // The host frees its own TX contexts on close; drop ours without echoing.
    memset(s_slots, 0, sizeof(s_slots));
    xQueueReset(s_echo_queue);
    xQueueReset(s_rx_queue);
    s_rx_overflow = false;
    s_state = TWAI_ERROR_ACTIVE;
}

esp_err_t can_bus_start(const can_timing_t *timing, const can_mode_t *mode)
{
    if (!can_bus_timing_valid(timing)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stop_locked();

    // Mode flags are creation-time options in the TWAI driver, so each start
    // builds a fresh node. The basic bitrate here is only a placeholder; the
    // host's exact timing is applied below.
    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = BOARD_CAN_TX_GPIO,
            .rx = BOARD_CAN_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = { .bitrate = 250000 },
        .fail_retry_cnt = mode->one_shot ? 0 : -1,
        .tx_queue_depth = TX_QUEUE_DEPTH,
        .flags = {
            .enable_listen_only = mode->listen_only,
            .enable_loopback = mode->loopback,
            .enable_self_test = mode->loopback,
        },
    };

    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) {
        s_node = NULL;
        goto out;
    }

    twai_timing_advanced_config_t adv = {
        .brp = timing->brp,
        .prop_seg = 0,
        .tseg_1 = (uint8_t)timing->tseg1,
        .tseg_2 = (uint8_t)timing->tseg2,
        .sjw = (uint8_t)timing->sjw,
        .ssp_offset = mode->triple_sample ? 1 : 0,
    };
    err = twai_node_reconfig_timing(s_node, &adv, NULL);
    if (err != ESP_OK) {
        goto fail;
    }

    twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
        .on_error = on_error,
    };
    err = twai_node_register_event_callbacks(s_node, &cbs, NULL);
    if (err != ESP_OK) {
        goto fail;
    }

    s_mode = *mode;
    err = twai_node_enable(s_node);
    if (err != ESP_OK) {
        goto fail;
    }
    ESP_LOGI(TAG, "started: brp=%lu tseg1=%lu tseg2=%lu sjw=%lu (%lu bit/s)%s%s%s",
             timing->brp, timing->tseg1, timing->tseg2, timing->sjw,
             CAN_FCLK_HZ / (timing->brp * (1 + timing->tseg1 + timing->tseg2)),
             mode->listen_only ? " listen-only" : "",
             mode->loopback ? " loopback" : "",
             mode->one_shot ? " one-shot" : "");
    goto out;

fail:
    twai_node_delete(s_node);
    s_node = NULL;
out:
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

void can_bus_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stop_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "stopped");
}

bool can_bus_running(void)
{
    return s_node != NULL;
}

static void echo_now(const gs_host_frame_t *frame)
{
    // Could not hand the frame to the controller: release the host's slot.
    s_stats.tx_failed++;
    xQueueSend(s_echo_queue, frame, 0);
}

void can_bus_send(const gs_host_frame_t *frame)
{
    if (frame->echo_id >= GS_MAX_TX_URBS) {
        ESP_LOGW(TAG, "echo_id %lu out of range", frame->echo_id);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    tx_slot_t *slot = &s_slots[frame->echo_id];
    if (!s_node || slot->in_use) {
        xSemaphoreGive(s_lock);
        echo_now(frame);
        return;
    }

    slot->echo = *frame;
    uint8_t dlc = frame->can_dlc > 8 ? 8 : frame->can_dlc;
    bool ext = frame->can_id & CAN_EFF_FLAG;
    bool rtr = frame->can_id & CAN_RTR_FLAG;
    memcpy(slot->data, frame->data, dlc);
    slot->frame = (twai_frame_t) {
        .header = {
            .id = ext ? (frame->can_id & CAN_EFF_MASK) : (frame->can_id & CAN_SFF_MASK),
            .dlc = dlc,
            .ide = ext,
            .rtr = rtr,
        },
        .buffer = slot->data,
        .buffer_len = rtr ? 0 : dlc,
    };
    slot->in_use = true;

    esp_err_t err = twai_node_transmit(s_node, &slot->frame, 0);
    if (err != ESP_OK) {
        slot->in_use = false;
        xSemaphoreGive(s_lock);
        echo_now(frame);
        return;
    }
    xSemaphoreGive(s_lock);
}

void can_bus_get_state(gs_device_state_t *out)
{
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_node) {
        out->state = GS_CAN_STATE_STOPPED;
    } else {
        twai_node_status_t st;
        if (twai_node_get_info(s_node, &st, NULL) == ESP_OK) {
            out->state = (uint32_t)st.state;    // TWAI and gs_usb state enums share order 0..3
            out->rxerr = st.rx_error_count;
            out->txerr = st.tx_error_count;
        }
    }
    xSemaphoreGive(s_lock);
}

void can_bus_get_stats(can_stats_t *out)
{
    *out = s_stats;
}
