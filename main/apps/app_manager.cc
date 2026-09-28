#include "app_manager.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "board.h"
#include "display.h"
#include <src/misc/cache/lv_cache.h>
#include "radar_app.h"
#include "flight_radar_app.h"

#define TAG "AppManager"

// Hide() fades back to the assistant screen in 300 ms; the app screen may only be
// deleted once that animation no longer references it.
static constexpr int kScreenAnimMs = 300;

AppManager& AppManager::GetInstance() {
    static AppManager instance;
    return instance;
}

// ลงทะเบียนแอปที่มากับ firmware — แอปใหม่เพิ่มที่นี่ที่เดียว
AppManager::AppManager() {
    Register({
        "radar",
        [] { RadarApp::GetInstance().Show(); },
        [] { RadarApp::GetInstance().Hide(); },
        [] { return RadarApp::GetInstance().IsVisible(); },
        [] { RadarApp::DestroyInstance(); },
    });
    Register({
        "flightradar",
        [] { FlightRadarApp::GetInstance().Show(); },
        [] { FlightRadarApp::GetInstance().Hide(); },
        [] { return FlightRadarApp::GetInstance().IsVisible(); },
        [] { FlightRadarApp::DestroyInstance(); },
    });
}

void AppManager::Register(App app) {
    std::lock_guard<std::mutex> lock(mutex_);
    ESP_LOGI(TAG, "register %s", app.name.c_str());
    apps_.push_back(std::move(app));
}

int AppManager::Find(const std::string& name) {
    for (int i = 0; i < (int)apps_.size(); i++) {
        if (apps_[i].name == name) return i;
    }
    return -1;
}

void AppManager::HideCurrentLocked() {
    if (current_ < 0) return;
    int idx = current_;
    ESP_LOGI(TAG, "close %s", apps_[idx].name.c_str());
    apps_[idx].hide();
    current_ = -1;
    vTaskDelay(pdMS_TO_TICKS(kScreenAnimMs + 50));
    ReleaseLocked(idx);
}

void AppManager::ReleaseLocked(int idx) {
    size_t before_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t before_ps  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    apps_[idx].release();
    {
        // decoded images of the closed app must not linger in the shared cache
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        lv_image_cache_drop(nullptr);
    }
    ESP_LOGI(TAG, "released %s: +%d B internal, +%d B PSRAM",
             apps_[idx].name.c_str(),
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (int)before_int,
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (int)before_ps);
}

void AppManager::ReleaseIfClosed(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    int idx = Find(name);
    if (idx >= 0 && idx != current_) ReleaseLocked(idx);
}

bool AppManager::Open(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    int idx = Find(name);
    if (idx < 0) {
        ESP_LOGW(TAG, "unknown app: %s", name.c_str());
        return false;
    }
    if (idx == current_ && apps_[idx].is_visible()) return true;

    // ปิดแอปเดิมก่อน → จอผู้ช่วยกลับมาเป็นจอที่กำลังโหลด
    // แอปใหม่จึงจำจอผู้ช่วยเป็นจอที่ต้องกลับ (ดู ReturnScreen ใน Show ของแต่ละแอป)
    HideCurrentLocked();

    ESP_LOGI(TAG, "open %s", name.c_str());
    apps_[idx].show();
    if (!apps_[idx].is_visible()) {
        ESP_LOGE(TAG, "open %s failed", name.c_str());
        ReleaseLocked(idx);
        return false;
    }
    current_ = idx;
    return true;
}

bool AppManager::Close(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    int idx = Find(name);
    if (idx < 0 || idx != current_) return false;
    HideCurrentLocked();
    return true;
}

bool AppManager::CloseCurrent() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_ < 0) return false;
    HideCurrentLocked();
    return true;
}

std::string AppManager::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_ < 0 ? std::string() : apps_[current_].name;
}

std::vector<std::string> AppManager::Names() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (auto& a : apps_) names.push_back(a.name);
    return names;
}
