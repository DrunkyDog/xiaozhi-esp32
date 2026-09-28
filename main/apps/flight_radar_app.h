/*
 * FlightRadarApp — แสดงเครื่องบินรอบตัวจากข้อมูล ADS-B สด
 *
 * แรงบันดาลใจ/แหล่งข้อมูลจาก socquique/capsule-radar (MIT) แต่**ไม่ได้ port โค้ด**
 * เพราะคนละ stack กันหมด (นั่นคือ LVGL v8 + Arduino_GFX + PlatformIO,
 * ของเราคือ LVGL v9 + esp_lcd + ESP-IDF) — เอามาเฉพาะ endpoint กับวิธีคำนวณ
 *
 * แหล่งข้อมูล (ฟรี ไม่ต้องมี API key):
 *   https://api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}
 *   fallback: https://api.adsb.lol/v2/point/...
 *   → JSON { "ac": [ {hex, flight, lat, lon, alt_baro, track, gs, ...} ] }
 *   ⚠️ ใช้เพื่อการศึกษา/ไม่เชิงพาณิชย์ และไม่ควร poll ถี่กว่า 1-2 วิ
 *
 * พิกัดบ้าน:
 *   - เก็บใน NVS (namespace "flightradar") เป็น int สเกล 1e5 — ไม่อยู่ในโค้ด
 *   - เปิดแอปครั้งแรกและยังไม่มีค่า → ดึงจาก IP geolocation ให้อัตโนมัติ
 *     (ทำตอนเปิดแอปเท่านั้น ไม่แตะตอน boot เพราะแอปไม่ได้ใช้บ่อย)
 *   - แก้เองได้ด้วย MCP: self.flightradar.set_home
 */
#ifndef FLIGHT_RADAR_APP_H
#define FLIGHT_RADAR_APP_H

#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "display.h"
#include "radar_scope.h"

#define FR_MAX_PLANES     24      // จำกัดจำนวนที่วาด เพื่อคุม RAM และความอ่านง่าย
#define FR_POLL_MS        4000    // ไม่ถี่เกินไป — เป็นพลเมืองดีของ API ฟรี
#define FR_DEFAULT_RANGE  40      // รัศมี (ไมล์ทะเล)
#define FR_JSON_BUF       (128 * 1024)   // alloc ใน PSRAM
#define FR_MAX_AIRPORTS   14      // สนามบินที่วาดพร้อมกันสูงสุด

class FlightRadarApp {
public:
    static FlightRadarApp& GetInstance();
    // ทำลาย instance (poll task, timer, screen, JSON buffer ใน PSRAM) — no-op ถ้ายังไม่เคยสร้าง
    static void DestroyInstance();
    ~FlightRadarApp();

    void Show();
    void Hide();
    bool IsVisible() const { return visible_; }

    // ตั้งพิกัดบ้านเอง → เก็บลง NVS ถาวร
    bool SetHome(float lat, float lon);
    bool HasHome() const { return has_home_; }
    // ดึงพิกัดจาก IP (เรียกตอนเปิดแอปถ้ายังไม่เคยตั้ง หรือสั่งเองเมื่อต้องการ)
    bool LocateByIp();
    // ดึงเฉพาะชื่อเมือง โดยไม่แตะพิกัดที่ตั้งไว้แล้ว
    bool FetchCityOnly();

    void SetRangeNm(int nm);
    int  RangeNm() const { return range_nm_; }

private:
    FlightRadarApp() = default;

    void BuildUi();
    void LoadHome();                       // อ่านจาก NVS
    static void PollTask(void* arg);
    bool FetchAndRender();
    void RenderPlanes(const char* json);
    void SetStatus(const char* text, uint32_t color);
    static void TimerCb(lv_timer_t* t);

    Display*    display_ = nullptr;
    bool        visible_ = false;
    bool        built_   = false;
    volatile bool running_ = false;
    TaskHandle_t task_   = nullptr;
    lv_timer_t*  timer_  = nullptr;

    RadarScope  scope_;
    lv_obj_t*   screen_       = nullptr;
    lv_obj_t*   prev_screen_  = nullptr;
    lv_obj_t*   status_label_ = nullptr;
    lv_obj_t*   count_label_  = nullptr;
    lv_obj_t*   city_label_   = nullptr;
    lv_obj_t*   coord_label_  = nullptr;

    // pool ของจุดเครื่องบิน + ป้ายชื่อ (สร้างครั้งเดียว ซ่อน/แสดงเอา)
    lv_obj_t*   plane_dot_[FR_MAX_PLANES]   = {};
    lv_obj_t*   plane_lbl_[FR_MAX_PLANES]   = {};

    // สนามบิน — ไม่เคลื่อนที่ จึง project แค่ตอนเปลี่ยนพิกัด/range ไม่ใช่ทุกรอบ
    // วาดเป็นสามเหลี่ยม ∆ ด้วย lv_line 4 จุด (ยอด → ซ้ายล่าง → ขวาล่าง → กลับยอด)
    void ProjectAirports();
    lv_obj_t*   ap_mark_[FR_MAX_AIRPORTS] = {};
    lv_obj_t*   ap_lbl_[FR_MAX_AIRPORTS]  = {};
    lv_point_precise_t ap_tri_[FR_MAX_AIRPORTS][4] = {};   // LVGL ถือ pointer → ต้องเป็น member

    void UpdateHomeLabels();               // อัปเดตป้าย city + lat/lon บนจอ

    float home_lat_ = 0.0f;
    float home_lon_ = 0.0f;
    char  home_city_[32] = {0};
    bool  has_home_ = false;
    bool  city_pending_ = false;      // มีพิกัดแล้วแต่ยังไม่รู้ชื่อเมือง → ดึงเพิ่มตอนเปิดแอป
    int   keepawake_tick_ = 0;        // นับ tick เพื่อกัน display auto-off
    int   range_nm_ = FR_DEFAULT_RANGE;
    char* json_buf_ = nullptr;             // PSRAM
};

#endif // FLIGHT_RADAR_APP_H
