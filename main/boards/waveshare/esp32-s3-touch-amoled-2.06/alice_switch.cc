#include "alice_switch.h"

#include <esp_log.h>
#include <initializer_list>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>

#define TAG "AliceSwitch"

namespace alice {
namespace {

// Shared with the Watch firmware (alice-watch-os core/power_policy.h)
constexpr const char* kNvsNamespace = "alice";
constexpr const char* kKeyVoicePaused = "voice_paused";
constexpr const char* kKeyWatchSlot = "watch_slot";  // label the Watch ran from before switching
constexpr uint64_t kSettleUs = 15 * 1000 * 1000;  // same pending-verify window as the Watch

esp_timer_handle_t s_timer = nullptr;

bool IsOtaSlot(const esp_partition_t* p, esp_partition_subtype_t a, esp_partition_subtype_t b) {
    return p != nullptr && (p->subtype == a || p->subtype == b);
}

bool Bootable(const esp_partition_t* p) {
    esp_app_desc_t desc;
    if (p == nullptr || esp_ota_get_partition_description(p, &desc) != ESP_OK) {
        return false;  // empty slot
    }
    esp_ota_img_states_t state;
    return !(esp_ota_get_state_partition(p, &state) == ESP_OK &&
             (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED));
}

// Watch slot to boot: the one the Watch recorded before switching to AI Voice (so an OTA'd
// Watch in ota_1 is not traded for an older ota_0), else the first bootable Watch slot.
const esp_partition_t* FindWatch() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
        char label[17] = "";
        size_t len = sizeof(label);
        esp_err_t err = nvs_get_str(h, kKeyWatchSlot, label, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            auto p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
            if (IsOtaSlot(p, ESP_PARTITION_SUBTYPE_APP_OTA_0, ESP_PARTITION_SUBTYPE_APP_OTA_1) && Bootable(p)) {
                return p;
            }
        }
    }
    const esp_partition_t* best = nullptr;
    for (auto sub : {ESP_PARTITION_SUBTYPE_APP_OTA_0, ESP_PARTITION_SUBTYPE_APP_OTA_1}) {
        auto p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, nullptr);
        if (best == nullptr && Bootable(p)) {
            best = p;
        }
    }
    return best;
}

bool VoicePaused() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t paused = 0;
    nvs_get_u8(h, kKeyVoicePaused, &paused);
    nvs_close(h);
    return paused != 0;
}

void OnSettled(void*) {
    esp_ota_mark_app_valid_cancel_rollback();
    auto watch = FindWatch();
    if (watch != nullptr && esp_ota_set_boot_partition(watch) == ESP_OK) {
        ESP_LOGI(TAG, "AI Voice marked valid; next boot returns to the Watch (%s)", watch->label);
    } else {
        ESP_LOGW(TAG, "No valid Watch image: next boot stays on AI Voice");
    }
}

}  // namespace

bool IsAliceLayout() {
    auto running = esp_ota_get_running_partition();
    return IsOtaSlot(running, ESP_PARTITION_SUBTYPE_APP_OTA_2, ESP_PARTITION_SUBTYPE_APP_OTA_3) &&
           esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr) != nullptr;
}

void Begin() {
    if (!IsAliceLayout() || s_timer != nullptr) {
        return;
    }
    if (VoicePaused()) {
        ESP_LOGW(TAG, "AI Voice is paused by the Watch's Battery Saver");
        esp_ota_mark_app_valid_cancel_rollback();  // this image is fine; it just must not run now
        ReturnToWatch();
    }
    esp_timer_create_args_t args = {};
    args.callback = OnSettled;
    args.name = "alice_settle";
    if (esp_timer_create(&args, &s_timer) == ESP_OK) {
        esp_timer_start_once(s_timer, kSettleUs);
    }
}

void ReturnToWatch() {
    if (!IsAliceLayout()) {
        return;
    }
    auto watch = FindWatch();
    if (watch == nullptr || esp_ota_set_boot_partition(watch) != ESP_OK) {
        ESP_LOGE(TAG, "No valid Watch image to return to");
        return;
    }
    ESP_LOGI(TAG, "Returning to the Watch (%s)", watch->label);
    esp_restart();
}

}  // namespace alice
