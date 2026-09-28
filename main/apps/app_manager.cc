#include "app_manager.h"

#include <esp_log.h>
#include "radar_app.h"
#include "flight_radar_app.h"

#define TAG "AppManager"

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
    });
    Register({
        "flightradar",
        [] { FlightRadarApp::GetInstance().Show(); },
        [] { FlightRadarApp::GetInstance().Hide(); },
        [] { return FlightRadarApp::GetInstance().IsVisible(); },
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
    ESP_LOGI(TAG, "close %s", apps_[current_].name.c_str());
    apps_[current_].hide();
    current_ = -1;
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
