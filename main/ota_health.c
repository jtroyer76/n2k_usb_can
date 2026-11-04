#include "ota_health.h"

#include "can_bus.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "gs_usb_dev.h"

static const char *TAG = "ota_health";

// Mark the image valid after it has been healthy and up this long...
#define HEALTHY_MARK_VALID_US   (60LL * 1000 * 1000)
// ...but if it never gets healthy by this long, restart to roll back.
#define ROLLBACK_TIMEOUT_US     (5LL * 60 * 1000 * 1000)
// USB counts as alive only if a transfer completed this recently.
#define USB_FRESH_US            (3LL * 1000 * 1000)

static bool s_pending;    // running image is awaiting verification
static bool s_resolved;   // already marked valid or triggered rollback

void ota_health_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        ESP_LOGW(TAG, "running a pending-verify image: must prove healthy within %lld s"
                      " or it rolls back", ROLLBACK_TIMEOUT_US / 1000000);
    }
}

// The new image is trustworthy only if the whole data path works: USB is
// enumerated AND actually moving data recently (a wedge left tud_mounted stuck
// true, so "mounted" alone is not enough - require real progress), and CAN is
// up, error-active, and has heard real traffic from other nodes.
static bool healthy(void)
{
    if (!gs_usb_dev_mounted() || !can_bus_running()) {
        return false;
    }
    if (esp_timer_get_time() - gs_usb_dev_last_progress_us() > USB_FRESH_US) {
        return false;
    }
    gs_device_state_t cs;
    can_bus_get_state(&cs);
    if (cs.state != GS_CAN_STATE_ERROR_ACTIVE || cs.txerr >= 96 || cs.rxerr >= 96) {
        return false;
    }
    can_stats_t stats;
    can_bus_get_stats(&stats);
    return stats.rx_frames > 0;   // heard real bus traffic since boot
}

void ota_health_poll(void)
{
    if (!s_pending || s_resolved) {
        return;
    }
    int64_t up = esp_timer_get_time();
    if (up >= HEALTHY_MARK_VALID_US && healthy()) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "image healthy; marked valid (rollback cancelled)");
            s_resolved = true;
        }
    } else if (up >= ROLLBACK_TIMEOUT_US) {
        ESP_LOGE(TAG, "image never proved healthy; restarting to roll back");
        s_resolved = true;
        esp_restart();   // bootloader sees PENDING_VERIFY -> reverts to previous image
    }
}

bool ota_health_pending(void)
{
    return s_pending && !s_resolved;
}
