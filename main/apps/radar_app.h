/*
 * RadarApp — หน้าจอ WiFi CSI radar สำหรับ xiaozhi-esp32 (TARS)
 * บอร์ด: waveshare esp32-s3-touch-amoled-2.06 (410 x 502, LVGL v9)
 *
 * รูปแบบ "แอปยกจอ":
 *   Show() -> สร้าง/โหลด lv screen ของตัวเองทับหน้าจอที่ TARS แสดงอยู่
 *   Hide() -> คืน screen เดิม (TARS กลับมาแสดงผลเหมือนเดิม)
 * โค้ดฝั่ง TARS ไม่ต้องรู้ว่าแอปนี้มีอยู่
 *
 * UI 4 หน้า (แนวคิดจาก ruvnet/RuView ADR-045, เขียนใหม่สำหรับ LVGL v9):
 *   0 DASHBOARD — วง radar + สถานะ
 *   1 VITALS    — อัตราการหายใจ + กราฟคลื่น
 *   2 PRESENCE  — กราฟ motion score + เส้น threshold
 *   3 SYSTEM    — อัตรา CSI, subcarrier, threshold, RAM
 *
 * เปลี่ยนหน้าได้ 3 ทาง: ปัดจอ / อัตโนมัติทุก 8 วิ / สั่งด้วยเสียง
 * (สองทางหลังจำเป็นเพราะทัชสกรีนอาจใช้ไม่ได้)
 *
 * Thread safety: ทุกเมธอดสาธารณะจับ DisplayLockGuard จึงเรียกจาก task อื่นได้
 */
#ifndef RADAR_APP_H
#define RADAR_APP_H

#include <lvgl.h>
#include "display.h"
#include "radar_scope.h"

#define RADAR_BLIP_POOL   16
#define RADAR_PAGES       4
#define RADAR_CHART_PTS   60      // จุดในกราฟ (60 จุด ~ ประวัติล่าสุด)
#define RADAR_ROTATE_MS   8000    // สลับหน้าอัตโนมัติทุก 8 วินาที

class RadarApp {
public:
    // สร้างครั้งแรกเมื่อเรียกใช้ (ต้องมี display แล้ว) — ใช้ผ่าน AppManager
    static RadarApp& GetInstance();
    // ทำลาย instance (screen, timer, หยุด sensor) — no-op ถ้ายังไม่เคยสร้าง
    static void DestroyInstance();

    explicit RadarApp(Display* display);
    ~RadarApp();

    void Show();
    void Hide();
    bool IsVisible() const { return visible_; }

    // ป้อนข้อมูล — thread-safe
    void Update(float score, bool present, bool calibrating = false);

    // เปลี่ยนหน้า (สั่งด้วยเสียงผ่าน MCP)
    void NextPage();
    void SetPage(int page);
    int  CurrentPage() const { return page_; }
    // เปิด/ปิดการสลับหน้าอัตโนมัติ
    void SetAutoRotate(bool on);
    bool AutoRotate() const { return auto_rotate_; }

private:
    void BuildUi();
    void BuildDashboard(lv_obj_t* t);
    void BuildVitals(lv_obj_t* t);
    void BuildPresence(lv_obj_t* t);
    void BuildSystem(lv_obj_t* t);
    void UpdateDots();
    void SpawnBlip(float score);
    static void TimerCb(lv_timer_t* t);
    void Tick();

    Display*   display_   = nullptr;
    bool       visible_   = false;
    bool       built_     = false;

    lv_obj_t*  screen_      = nullptr;
    lv_obj_t*  prev_screen_ = nullptr;
    lv_obj_t*  tileview_    = nullptr;
    lv_obj_t*  tile_[RADAR_PAGES]  = {};
    lv_obj_t*  dot_[RADAR_PAGES]   = {};   // จุดบอกหน้าปัจจุบัน
    lv_timer_t* timer_      = nullptr;

    RadarScope scope_;

    // หน้า 0
    lv_obj_t*  status_pill_  = nullptr;
    lv_obj_t*  status_dot_   = nullptr;
    lv_obj_t*  status_label_ = nullptr;
    lv_obj_t*  score_label_  = nullptr;
    lv_obj_t*  meter_        = nullptr;

    // หน้า 1 (vitals)
    lv_obj_t*  bpm_label_    = nullptr;
    lv_obj_t*  bpm_hint_     = nullptr;
    lv_obj_t*  vit_chart_    = nullptr;
    lv_chart_series_t* vit_ser_ = nullptr;

    // หน้า 2 (presence)
    lv_obj_t*  pres_chart_   = nullptr;
    lv_chart_series_t* pres_ser_ = nullptr;
    lv_obj_t*  pres_state_   = nullptr;

    // หน้า 3 (system)
    lv_obj_t*  sys_rate_ = nullptr;
    lv_obj_t*  sys_sub_  = nullptr;
    lv_obj_t*  sys_thr_  = nullptr;
    lv_obj_t*  sys_ram_  = nullptr;

    struct Blip { lv_obj_t* obj = nullptr; uint32_t born = 0; bool active = false; };
    Blip blips_[RADAR_BLIP_POOL];
    int  blip_next_ = 0;

    int  page_ = 0;
    bool auto_rotate_ = true;
    int  rotate_tick_ = 0;
    int  keepawake_tick_ = 0;
    int  sys_tick_ = 0;
};

#endif // RADAR_APP_H
