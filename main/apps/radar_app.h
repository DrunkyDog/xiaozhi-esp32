/*
 * RadarApp — LVGL radar screen for xiaozhi-esp32 (Alice)
 * Target board: waveshare esp32-s3-touch-amoled-2.06  (410 x 502, LVGL v9)
 *
 * "App takes over the screen" pattern:
 *   Show()  -> creates/loads its OWN lv screen over whatever Alice is showing
 *   Hide()  -> restores the previously active screen (Alice UI returns)
 * Alice's own display code never needs to know this app exists.
 *
 * Rendering uses plain LVGL widgets (arcs/lines/objs), NOT lv_canvas:
 * a full-screen canvas at 410x502 RGB565 would cost ~411 KB of RAM.
 *
 * Thread safety: every public method takes DisplayLockGuard, so it is safe
 * to call Update() from a network task.
 */
#ifndef RADAR_APP_H
#define RADAR_APP_H

#include <lvgl.h>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "display.h"

#define RADAR_BLIP_POOL   16
#define RADAR_SPOKES      12

// ข้อมูลมาจาก CsiSensor บนเครื่องโดยตรง — ไม่มี backend แล้ว

class RadarApp {
public:
    explicit RadarApp(Display* display);
    ~RadarApp();

    // แสดง radar ทับหน้าจอปัจจุบัน (จำ screen เดิมไว้คืนทีหลัง)
    void Show();
    // ปิด radar แล้วคืนหน้าจอเดิมให้ Alice
    void Hide();
    bool IsVisible() const { return visible_; }

    // ป้อนข้อมูล: score = motion score, present = มีคนไหม, calibrating = กำลังเก็บ baseline
    // thread-safe — เรียกจาก task อื่นได้
    void Update(float score, bool present, bool calibrating = false);

private:
    void BuildUi();                       // สร้าง widget ครั้งเดียว (lazy)
    void SpawnBlip(float score);
    static void TimerCb(lv_timer_t* t);   // หมุน sweep + fade blips
    void Tick();

    Display*   display_   = nullptr;
    bool       visible_   = false;
    bool       built_     = false;

    lv_obj_t*  screen_    = nullptr;      // screen ของ radar เอง
    lv_obj_t*  prev_screen_ = nullptr;    // screen ของ Alice ที่จะคืนกลับ
    lv_timer_t* timer_    = nullptr;

    lv_obj_t*  sweep_line_ = nullptr;
    lv_obj_t*  status_pill_ = nullptr;
    lv_obj_t*  status_dot_  = nullptr;
    lv_obj_t*  status_label_ = nullptr;
    lv_obj_t*  score_label_ = nullptr;
    lv_obj_t*  meter_       = nullptr;

    // LVGL เก็บเป็น pointer → array ต้องอยู่ยาว (เป็น member ห้ามเป็น local)
    lv_point_precise_t sweep_pts_[2];
    lv_point_precise_t spoke_pts_[RADAR_SPOKES][2];

    struct Blip {
        lv_obj_t* obj = nullptr;
        uint32_t  born = 0;
        bool      active = false;
    };
    Blip blips_[RADAR_BLIP_POOL];
    int  blip_next_ = 0;

    float angle_ = 0.0f;                  // มุม sweep ปัจจุบัน (rad)
};

#endif // RADAR_APP_H
