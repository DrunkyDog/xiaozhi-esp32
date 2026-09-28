#include "radar_app.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include "csi_sensor.h"
#include "board.h"
#include "app_manager.h"

#define TAG "RadarApp"

// --- layout: 410 x 502 portrait ---
#define SCR_W   410
#define SCR_H   502
#define CX      205
#define CY      190
#define RADIUS  150

// ธีมเดียวกับ RuView: พื้นเกือบดำ + accent ฟ้า cyan
#define COL_BG     0x0A0A0F
#define COL_PANEL  0x121722
#define COL_LINE   0x1E2A3A
#define COL_CYAN   0x00D4FF
#define COL_GREEN  0x16D3A2
#define COL_RED    0xFF4D5E
#define COL_TEXT   0xC9D6E5
#define COL_MUTED  0x5F7086

#define BLIP_LIFE_MS 2600
#define TICK_MS      33          // ~30 fps

static RadarApp* s_radar = nullptr;

RadarApp& RadarApp::GetInstance() {
    if (s_radar == nullptr) s_radar = new RadarApp(Board::GetInstance().GetDisplay());
    return *s_radar;
}

void RadarApp::DestroyInstance() {
    delete s_radar;          // ~RadarApp stops the sensor, deletes timer + screen
    s_radar = nullptr;
}

RadarApp::RadarApp(Display* display) : display_(display) {}

RadarApp::~RadarApp() {
    // หยุด sensor ให้จบก่อนจับ display lock — ไม่งั้น deadlock
    // (proc task ของ sensor เรียก Update() ซึ่งจับ lock ตัวเดียวกัน)
    CsiSensor::GetInstance().Stop();
    vTaskDelay(pdMS_TO_TICKS(250));

    DisplayLockGuard lock(display_);
    if (timer_)  { lv_timer_delete(timer_); timer_ = nullptr; }
    if (screen_) { lv_obj_delete(screen_);  screen_ = nullptr; }
}

// ล้าง style ตั้งต้นของ lv_obj ให้เป็นกล่องใส
static void Strip(lv_obj_t* o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
}

// การ์ดพื้นหลังแบบเดียวกันทุกหน้า
static lv_obj_t* Card(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* c = lv_obj_create(parent);
    Strip(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_LINE), 0);
    return c;
}

static lv_obj_t* Label(lv_obj_t* p, const char* txt, uint32_t color,
                       lv_align_t align, int x, int y) {
    lv_obj_t* l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, align, x, y);
    return l;
}

// กราฟเส้นธีมเดียวกัน
static lv_obj_t* Chart(lv_obj_t* p, int x, int y, int w, int h,
                       uint32_t color, lv_chart_series_t** out_ser) {
    lv_obj_t* ch = lv_chart_create(p);
    lv_obj_set_size(ch, w, h);
    lv_obj_set_pos(ch, x, y);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, RADAR_CHART_PTS);
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_line_color(ch, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);      // ไม่ต้องมีจุด
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    lv_obj_remove_flag(ch, LV_OBJ_FLAG_SCROLLABLE);
    *out_ser = lv_chart_add_series(ch, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
    return ch;
}

// ---------------- หน้า 0: DASHBOARD ----------------
void RadarApp::BuildDashboard(lv_obj_t* t) {
    Label(t, "WIFI CSI RADAR", COL_MUTED, LV_ALIGN_TOP_MID, 0, 6);
    scope_.Build(t, CX, CY, RADIUS, SCR_W, SCR_H, COL_CYAN);

    for (int i = 0; i < RADAR_BLIP_POOL; i++) {
        lv_obj_t* b = lv_obj_create(t);
        Strip(b);
        lv_obj_set_size(b, 9, 9);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_RED), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        blips_[i].obj = b;
    }

    status_pill_ = Card(t, 16, 356, 240, 52);
    status_dot_  = lv_obj_create(status_pill_);
    Strip(status_dot_);
    lv_obj_set_size(status_dot_, 14, 14);
    lv_obj_align(status_dot_, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_dot_, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, 0);
    status_label_ = Label(status_pill_, "CLEAR", COL_TEXT, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t* sb = Card(t, 266, 356, 128, 52);
    Label(sb, "MOTION", COL_MUTED, LV_ALIGN_TOP_MID, 0, 4);
    score_label_ = Label(sb, "0.00", COL_TEXT, LV_ALIGN_BOTTOM_MID, 0, -4);

    meter_ = lv_bar_create(t);
    lv_obj_set_size(meter_, 378, 8);
    lv_obj_set_pos(meter_, 16, 420);
    lv_bar_set_range(meter_, 0, 100);
    lv_bar_set_value(meter_, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(meter_, 4, 0);
    lv_obj_set_style_bg_color(meter_, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(meter_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(meter_, lv_color_hex(COL_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(meter_, LV_OPA_COVER, LV_PART_INDICATOR);
}

// ---------------- หน้า 1: VITALS ----------------
void RadarApp::BuildVitals(lv_obj_t* t) {
    Label(t, "VITALS", COL_MUTED, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t* c = Card(t, 16, 40, 378, 150);
    Label(c, "BREATHING", COL_MUTED, LV_ALIGN_TOP_MID, 0, 10);
    bpm_label_ = Label(c, "--", COL_CYAN, LV_ALIGN_CENTER, 0, 6);
    bpm_hint_  = Label(c, "BPM", COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_t* c2 = Card(t, 16, 202, 378, 200);
    Label(c2, "waveform 0.1-0.5 Hz", COL_MUTED, LV_ALIGN_TOP_LEFT, 12, 8);
    vit_chart_ = Chart(c2, 12, 34, 354, 150, COL_CYAN, &vit_ser_);
    lv_chart_set_axis_range(vit_chart_, LV_CHART_AXIS_PRIMARY_Y, -100, 100);

    Label(t, "นั่งนิ่ง ๆ ~30 วินาทีเพื่อวัด", COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -50);
}

// ---------------- หน้า 2: PRESENCE ----------------
void RadarApp::BuildPresence(lv_obj_t* t) {
    Label(t, "PRESENCE", COL_MUTED, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t* c = Card(t, 16, 40, 378, 80);
    pres_state_ = Label(c, "CLEAR", COL_GREEN, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* c2 = Card(t, 16, 132, 378, 270);
    Label(c2, "motion score", COL_MUTED, LV_ALIGN_TOP_LEFT, 12, 8);
    pres_chart_ = Chart(c2, 12, 34, 354, 220, COL_GREEN, &pres_ser_);
    lv_chart_set_axis_range(pres_chart_, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
}

// ---------------- หน้า 3: SYSTEM ----------------
void RadarApp::BuildSystem(lv_obj_t* t) {
    Label(t, "SYSTEM", COL_MUTED, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t* c = Card(t, 16, 40, 378, 300);
    Label(c, "CSI rate",    COL_MUTED, LV_ALIGN_TOP_LEFT, 16, 24);
    sys_rate_ = Label(c, "-", COL_TEXT, LV_ALIGN_TOP_RIGHT, -16, 24);
    Label(c, "subcarriers", COL_MUTED, LV_ALIGN_TOP_LEFT, 16, 84);
    sys_sub_  = Label(c, "-", COL_TEXT, LV_ALIGN_TOP_RIGHT, -16, 84);
    Label(c, "threshold",   COL_MUTED, LV_ALIGN_TOP_LEFT, 16, 144);
    sys_thr_  = Label(c, "-", COL_TEXT, LV_ALIGN_TOP_RIGHT, -16, 144);
    Label(c, "free RAM",    COL_MUTED, LV_ALIGN_TOP_LEFT, 16, 204);
    sys_ram_  = Label(c, "-", COL_TEXT, LV_ALIGN_TOP_RIGHT, -16, 204);
}

// ---------------- โครงหลัก ----------------
void RadarApp::BuildUi() {
    if (built_) return;

    screen_ = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen_, 0, 0);

    tileview_ = lv_tileview_create(screen_);
    lv_obj_set_size(tileview_, SCR_W, SCR_H - 22);   // เว้นที่ให้จุดบอกหน้า
    lv_obj_set_pos(tileview_, 0, 0);
    lv_obj_set_style_bg_opa(tileview_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tileview_, 0, 0);

    // 4 หน้าเรียงแนวนอน — ปัดซ้าย/ขวาได้ (ถ้าทัชใช้ได้)
    for (int i = 0; i < RADAR_PAGES; i++) {
        lv_dir_t dir = (lv_dir_t)((i > 0 ? LV_DIR_LEFT : 0) |
                                  (i < RADAR_PAGES - 1 ? LV_DIR_RIGHT : 0));
        tile_[i] = lv_tileview_add_tile(tileview_, i, 0, dir);
        lv_obj_set_style_bg_opa(tile_[i], LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(tile_[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    BuildDashboard(tile_[0]);
    BuildVitals(tile_[1]);
    BuildPresence(tile_[2]);
    BuildSystem(tile_[3]);

    // จุดบอกหน้าปัจจุบัน (ล่างสุด)
    for (int i = 0; i < RADAR_PAGES; i++) {
        lv_obj_t* d = lv_obj_create(screen_);
        Strip(d);
        lv_obj_set_size(d, 7, 7);
        lv_obj_set_pos(d, SCR_W / 2 - 26 + i * 16, SCR_H - 16);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_40, 0);
        dot_[i] = d;
    }
    UpdateDots();
    built_ = true;
    ESP_LOGI(TAG, "UI built (%dx%d, %d หน้า)", SCR_W, SCR_H, RADAR_PAGES);
}

void RadarApp::UpdateDots() {
    for (int i = 0; i < RADAR_PAGES; i++) {
        if (!dot_[i]) continue;
        bool on = (i == page_);
        lv_obj_set_style_bg_color(dot_[i], lv_color_hex(on ? COL_CYAN : COL_MUTED), 0);
        lv_obj_set_style_bg_opa(dot_[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

// ---------------- เปลี่ยนหน้า ----------------
void RadarApp::SetPage(int page) {
    if (page < 0) page = 0;
    if (page >= RADAR_PAGES) page = RADAR_PAGES - 1;
    DisplayLockGuard lock(display_);
    if (!built_) return;
    page_ = page;
    lv_tileview_set_tile_by_index(tileview_, page_, 0, LV_ANIM_ON);
    UpdateDots();
    rotate_tick_ = 0;                 // สั่งเองแล้ว เริ่มนับ auto ใหม่
}

void RadarApp::NextPage() { SetPage((page_ + 1) % RADAR_PAGES); }

void RadarApp::SetAutoRotate(bool on) {
    auto_rotate_ = on;
    rotate_tick_ = 0;
    ESP_LOGI(TAG, "auto-rotate %s", on ? "เปิด" : "ปิด");
}

// ---------------- lifecycle ----------------
void RadarApp::Show() {
    DisplayLockGuard lock(display_);
    if (visible_) return;
    BuildUi();

    prev_screen_ = AppManager::ReturnScreen();
    lv_screen_load_anim(screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, 300, 0, false);
    if (!timer_) timer_ = lv_timer_create(TimerCb, TICK_MS, this);
    lv_timer_resume(timer_);
    visible_ = true;

    bool ok = CsiSensor::GetInstance().Start([this](float s, bool p, bool c) {
        this->Update(s, p, c);
    });
    if (!ok) {
        lv_label_set_text(status_label_, "CSI ERROR");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_RED), 0);
        ESP_LOGE(TAG, "CSI sensor เริ่มไม่ได้");
    }
    ESP_LOGI(TAG, "shown");
}

void RadarApp::Hide() {
    DisplayLockGuard lock(display_);
    if (!visible_) return;
    CsiSensor::GetInstance().Stop();
    if (timer_) lv_timer_pause(timer_);
    if (prev_screen_) {
        lv_screen_load_anim(prev_screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, 300, 0, false);
    }
    visible_ = false;
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::BALANCED);
    ESP_LOGI(TAG, "hidden");
}

// ---------------- ป้อนข้อมูล ----------------
void RadarApp::Update(float score, bool present, bool calibrating) {
    DisplayLockGuard lock(display_);
    if (!built_) return;

    // ตัวเลข score — nano printf ไม่รองรับ %f จึงแยกส่วนเอง
    int w = (int)score;
    int f2 = (int)((score - (float)w) * 100.0f + 0.5f);
    if (f2 > 99) { w += 1; f2 = 0; }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d.%02d", w, f2);
    lv_label_set_text(score_label_, buf);

    int pct = (int)(score / 6.0f * 100.0f);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    lv_bar_set_value(meter_, pct, LV_ANIM_ON);
    if (pres_ser_) lv_chart_set_next_value(pres_chart_, pres_ser_, pct);

    // กราฟคลื่นหายใจ (สเกลให้พอเห็น)
    auto& s = CsiSensor::GetInstance();
    if (vit_ser_) {
        int v = (int)(score * 20.0f) - 50;
        if (v > 100) v = 100;
        if (v < -100) v = -100;
        lv_chart_set_next_value(vit_chart_, vit_ser_, v);
    }

    // อัตราการหายใจ
    float bpm = s.BreathingBpm();
    if (bpm > 0.0f) {
        char bb[12];
        snprintf(bb, sizeof(bb), "%d", (int)(bpm + 0.5f));
        lv_label_set_text(bpm_label_, bb);
    } else {
        lv_label_set_text(bpm_label_, "--");
    }

    if (calibrating) {
        lv_label_set_text(status_label_, "CALIBRATING");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_bg_color(status_dot_, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_border_color(status_pill_, lv_color_hex(COL_LINE), 0);
        if (pres_state_) {
            lv_label_set_text(pres_state_, "CALIBRATING");
            lv_obj_set_style_text_color(pres_state_, lv_color_hex(COL_MUTED), 0);
        }
        return;
    }

    uint32_t col = present ? COL_RED : COL_GREEN;
    lv_label_set_text(status_label_, present ? "MOTION" : "CLEAR");
    lv_obj_set_style_text_color(status_label_, lv_color_hex(present ? COL_RED : COL_TEXT), 0);
    lv_obj_set_style_bg_color(status_dot_, lv_color_hex(col), 0);
    lv_obj_set_style_border_color(status_pill_, lv_color_hex(present ? COL_RED : COL_LINE), 0);
    if (pres_state_) {
        lv_label_set_text(pres_state_, present ? "MOTION" : "CLEAR");
        lv_obj_set_style_text_color(pres_state_, lv_color_hex(col), 0);
    }
    if (present) SpawnBlip(score);
}

void RadarApp::SpawnBlip(float score) {
    Blip& b = blips_[blip_next_];
    blip_next_ = (blip_next_ + 1) % RADAR_BLIP_POOL;
    float a = ((float)(esp_random() % 3600) / 3600.0f) * 2.0f * (float)M_PI;
    float k = score / 8.0f; if (k > 1.0f) k = 1.0f;
    float dist = RADIUS * (0.2f + 0.8f * k);
    lv_obj_set_pos(b.obj, (int)(CX + dist * cosf(a)) - 4, (int)(CY + dist * sinf(a)) - 4);
    lv_obj_remove_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(b.obj, LV_OPA_COVER, 0);
    b.born = lv_tick_get();
    b.active = true;
}

// ---------------- timer ----------------
void RadarApp::TimerCb(lv_timer_t* t) {
    static_cast<RadarApp*>(lv_timer_get_user_data(t))->Tick();
}

void RadarApp::Tick() {
    scope_.TickSweep(0.09f);

    uint32_t now = lv_tick_get();
    for (int i = 0; i < RADAR_BLIP_POOL; i++) {
        Blip& b = blips_[i];
        if (!b.active) continue;
        uint32_t age = now - b.born;
        if (age >= BLIP_LIFE_MS) {
            lv_obj_add_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
            b.active = false;
        } else {
            lv_obj_set_style_bg_opa(b.obj, (lv_opa_t)(255 - age * 255 / BLIP_LIFE_MS), 0);
        }
    }

    // สลับหน้าอัตโนมัติ — จำเป็นเพราะทัชอาจใช้ไม่ได้
    if (auto_rotate_ && ++rotate_tick_ >= RADAR_ROTATE_MS / TICK_MS) {
        rotate_tick_ = 0;
        page_ = (page_ + 1) % RADAR_PAGES;
        lv_tileview_set_tile_by_index(tileview_, page_, 0, LV_ANIM_ON);
        UpdateDots();
    }

    // อัปเดตหน้า SYSTEM ทุก ~1 วิ
    if (++sys_tick_ >= 1000 / TICK_MS) {
        sys_tick_ = 0;
        auto& s = CsiSensor::GetInstance();
        char b[24];
        snprintf(b, sizeof(b), "%d Hz", (int)(s.CsiRateHz() + 0.5f));
        if (sys_rate_) lv_label_set_text(sys_rate_, b);
        snprintf(b, sizeof(b), "%d", s.Subcarriers());
        if (sys_sub_) lv_label_set_text(sys_sub_, b);
        float th = s.ThresholdHigh();
        snprintf(b, sizeof(b), "%d.%02d", (int)th, (int)((th - (int)th) * 100));
        if (sys_thr_) lv_label_set_text(sys_thr_, b);
        snprintf(b, sizeof(b), "%d KB", (int)(esp_get_free_heap_size() / 1024));
        if (sys_ram_) lv_label_set_text(sys_ram_, b);
    }

    // กันจอดับระหว่างใช้แอป (SetPowerSaveLevel เรียก WakeUp() → ticks_ = 0)
    if (++keepawake_tick_ >= 10000 / TICK_MS) {
        keepawake_tick_ = 0;
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }
}
