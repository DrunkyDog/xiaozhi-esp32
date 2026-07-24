#include "radar_app.h"

#include <esp_log.h>
#include <esp_random.h>
#include "csi_sensor.h"
#include <math.h>
#include <stdio.h>

#define TAG "RadarApp"

// --- layout: waveshare esp32-s3-touch-amoled-2.06 = 410 x 502 portrait ---
#define SCR_W      410
#define SCR_H      502
#define CX         205          // radar center x
#define CY         185          // radar center y
#define RADIUS     140          // radar outer radius

// --- palette (ตรงกับ frontend web) ---
#define COL_BG     0x0A0E14
#define COL_PANEL  0x111823
#define COL_LINE   0x1E2A3A
#define COL_GREEN  0x16D3A2
#define COL_RED    0xFF4D5E
#define COL_TEXT   0xC9D6E5
#define COL_MUTED  0x5F7086

#define BLIP_LIFE_MS 2600
#define TICK_MS      33         // ~30 fps

RadarApp::RadarApp(Display* display) : display_(display) {}

RadarApp::~RadarApp() {
    // ต้องหยุด feed task ให้จบก่อน แล้วค่อยจับ display lock
    // (ถ้าจับ lock ก่อนแล้วรอ task → deadlock เพราะ task ต้องการ lock เดียวกันใน Update())
    CsiSensor::GetInstance().Stop();
    vTaskDelay(pdMS_TO_TICKS(250));         // ให้ task ของ sensor คลายตัวก่อน

    DisplayLockGuard lock(display_);
    if (timer_)  { lv_timer_delete(timer_); timer_ = nullptr; }
    if (screen_) { lv_obj_delete(screen_);  screen_ = nullptr; }
}

// helper: ล้าง style ตั้งต้นของ lv_obj (bg/border/pad) ให้เป็นกล่องใส
static void StripObj(lv_obj_t* o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
}

void RadarApp::BuildUi() {
    if (built_) return;

    // ---- screen ของ radar เอง ----
    screen_ = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen_, 0, 0);

    // ---- title ----
    lv_obj_t* title = lv_label_create(screen_);
    lv_label_set_text(title, "WIFI CSI RADAR");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // ---- rings (4 วง) ----
    for (int i = 1; i <= 4; i++) {
        int d = (RADIUS * 2) * i / 4;
        lv_obj_t* ring = lv_obj_create(screen_);
        StripObj(ring);
        lv_obj_set_size(ring, d, d);
        lv_obj_set_pos(ring, CX - d / 2, CY - d / 2);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_20, 0);
    }

    // ---- spokes (12 เส้น ทุก 30°) ----
    for (int i = 0; i < RADAR_SPOKES; i++) {
        float a = (float)i * (2.0f * (float)M_PI / RADAR_SPOKES);
        spoke_pts_[i][0].x = CX;
        spoke_pts_[i][0].y = CY;
        spoke_pts_[i][1].x = (lv_value_precise_t)(CX + RADIUS * cosf(a));
        spoke_pts_[i][1].y = (lv_value_precise_t)(CY + RADIUS * sinf(a));

        lv_obj_t* spoke = lv_line_create(screen_);
        StripObj(spoke);
        lv_obj_set_size(spoke, SCR_W, SCR_H);
        lv_obj_set_pos(spoke, 0, 0);
        lv_line_set_points(spoke, spoke_pts_[i], 2);
        lv_obj_set_style_line_width(spoke, 1, 0);
        lv_obj_set_style_line_color(spoke, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_style_line_opa(spoke, LV_OPA_20, 0);
    }

    // ---- blips pool (ซ่อนไว้ก่อน) ----
    for (int i = 0; i < RADAR_BLIP_POOL; i++) {
        lv_obj_t* b = lv_obj_create(screen_);
        StripObj(b);
        lv_obj_set_size(b, 9, 9);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        blips_[i].obj = b;
        blips_[i].active = false;
    }

    // ---- sweep line (อยู่บนสุดของ radar) ----
    sweep_pts_[0].x = CX;  sweep_pts_[0].y = CY;
    sweep_pts_[1].x = CX + RADIUS;  sweep_pts_[1].y = CY;
    sweep_line_ = lv_line_create(screen_);
    StripObj(sweep_line_);
    lv_obj_set_size(sweep_line_, SCR_W, SCR_H);
    lv_obj_set_pos(sweep_line_, 0, 0);
    lv_line_set_points(sweep_line_, sweep_pts_, 2);
    lv_obj_set_style_line_width(sweep_line_, 2, 0);
    lv_obj_set_style_line_color(sweep_line_, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_line_opa(sweep_line_, LV_OPA_COVER, 0);

    // ---- status pill ----
    status_pill_ = lv_obj_create(screen_);
    StripObj(status_pill_);
    lv_obj_set_size(status_pill_, 250, 52);
    lv_obj_set_pos(status_pill_, 16, 340);
    lv_obj_set_style_radius(status_pill_, 12, 0);
    lv_obj_set_style_bg_color(status_pill_, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(status_pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_pill_, 1, 0);
    lv_obj_set_style_border_color(status_pill_, lv_color_hex(COL_LINE), 0);

    status_dot_ = lv_obj_create(status_pill_);
    StripObj(status_dot_);
    lv_obj_set_size(status_dot_, 14, 14);
    lv_obj_align(status_dot_, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_dot_, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, 0);

    status_label_ = lv_label_create(status_pill_);
    lv_label_set_text(status_label_, "CLEAR");
    lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(status_label_, LV_ALIGN_LEFT_MID, 40, 0);

    // ---- score box ----
    lv_obj_t* score_box = lv_obj_create(screen_);
    StripObj(score_box);
    lv_obj_set_size(score_box, 128, 52);
    lv_obj_set_pos(score_box, 274, 340);
    lv_obj_set_style_radius(score_box, 12, 0);
    lv_obj_set_style_bg_color(score_box, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(score_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(score_box, 1, 0);
    lv_obj_set_style_border_color(score_box, lv_color_hex(COL_LINE), 0);

    lv_obj_t* score_cap = lv_label_create(score_box);
    lv_label_set_text(score_cap, "MOTION");
    lv_obj_set_style_text_color(score_cap, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(score_cap, LV_ALIGN_TOP_MID, 0, 4);

    score_label_ = lv_label_create(score_box);
    lv_label_set_text(score_label_, "0.00");
    lv_obj_set_style_text_color(score_label_, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(score_label_, LV_ALIGN_BOTTOM_MID, 0, -4);

    // ---- motion meter ----
    meter_ = lv_bar_create(screen_);
    lv_obj_set_size(meter_, 378, 8);
    lv_obj_set_pos(meter_, 16, 404);
    lv_bar_set_range(meter_, 0, 100);
    lv_bar_set_value(meter_, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(meter_, 4, 0);
    lv_obj_set_style_bg_color(meter_, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(meter_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(meter_, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(meter_, LV_OPA_COVER, LV_PART_INDICATOR);

    built_ = true;
    ESP_LOGI(TAG, "UI built (%dx%d)", SCR_W, SCR_H);
}

void RadarApp::Show() {
    DisplayLockGuard lock(display_);
    if (visible_) return;
    BuildUi();

    prev_screen_ = lv_screen_active();                 // จำหน้าจอ Alice ไว้
    lv_screen_load_anim(screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, 300, 0, false);

    if (!timer_) timer_ = lv_timer_create(TimerCb, TICK_MS, this);
    lv_timer_resume(timer_);
    visible_ = true;
    // เริ่มตรวจจับ CSI บนเครื่อง (ไม่ต้องมี backend)
    bool ok = CsiSensor::GetInstance().Start([this](float score, bool present, bool calib) {
        this->Update(score, present, calib);
    });
    if (!ok) {
        // ไม่ crash — บอกบนจอตรง ๆ ว่า sensor ไม่ทำงาน
        lv_label_set_text(status_label_, "CSI ERROR");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_bg_color(status_dot_, lv_color_hex(COL_RED), 0);
        ESP_LOGE(TAG, "CSI sensor เริ่มไม่ได้ — radar จะแสดงผลเปล่า");
    }
    ESP_LOGI(TAG, "shown");
}

void RadarApp::Hide() {
    DisplayLockGuard lock(display_);
    if (!visible_) return;

    CsiSensor::GetInstance().Stop();                    // หยุดตรวจจับ ไม่กิน CPU/traffic เปล่า
    if (timer_) lv_timer_pause(timer_);
    if (prev_screen_) {                                 // คืนหน้าจอให้ Alice
        lv_screen_load_anim(prev_screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, 300, 0, false);
    }
    visible_ = false;
    ESP_LOGI(TAG, "hidden");
}

void RadarApp::Update(float score, bool present, bool calibrating) {
    DisplayLockGuard lock(display_);
    if (!built_) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", score);
    lv_label_set_text(score_label_, buf);

    int pct = (int)(score / 6.0f * 100.0f);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    lv_bar_set_value(meter_, pct, LV_ANIM_ON);

    if (calibrating) {
        lv_label_set_text(status_label_, "CALIBRATING");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_bg_color(status_dot_, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_border_color(status_pill_, lv_color_hex(COL_LINE), 0);
        return;
    }

    if (present) {
        lv_label_set_text(status_label_, "MOTION");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_bg_color(status_dot_, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_border_color(status_pill_, lv_color_hex(COL_RED), 0);
        lv_obj_set_style_bg_color(meter_, lv_color_hex(COL_RED), LV_PART_INDICATOR);
        SpawnBlip(score);
    } else {
        lv_label_set_text(status_label_, "CLEAR");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_bg_color(status_dot_, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_style_border_color(status_pill_, lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_bg_color(meter_, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    }
}

void RadarApp::SpawnBlip(float score) {
    Blip& b = blips_[blip_next_];
    blip_next_ = (blip_next_ + 1) % RADAR_BLIP_POOL;

    // สุ่มมุม, ระยะตามความแรงสัญญาณ
    float a = ((float)(esp_random() % 3600) / 3600.0f) * 2.0f * (float)M_PI;
    float k = score / 8.0f;
    if (k > 1.0f) k = 1.0f;
    float dist = RADIUS * (0.2f + 0.8f * k);

    int x = (int)(CX + dist * cosf(a)) - 4;
    int y = (int)(CY + dist * sinf(a)) - 4;
    lv_obj_set_pos(b.obj, x, y);
    lv_obj_remove_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(b.obj, LV_OPA_COVER, 0);
    b.born = lv_tick_get();
    b.active = true;
}

void RadarApp::TimerCb(lv_timer_t* t) {
    static_cast<RadarApp*>(lv_timer_get_user_data(t))->Tick();
}

void RadarApp::Tick() {
    // หมุนเส้นกวาด
    angle_ += 0.09f;
    if (angle_ > 2.0f * (float)M_PI) angle_ -= 2.0f * (float)M_PI;
    sweep_pts_[1].x = (lv_value_precise_t)(CX + RADIUS * cosf(angle_));
    sweep_pts_[1].y = (lv_value_precise_t)(CY + RADIUS * sinf(angle_));
    lv_line_set_points(sweep_line_, sweep_pts_, 2);

    // fade blips ตามอายุ
    uint32_t now = lv_tick_get();
    for (int i = 0; i < RADAR_BLIP_POOL; i++) {
        Blip& b = blips_[i];
        if (!b.active) continue;
        uint32_t age = now - b.born;
        if (age >= BLIP_LIFE_MS) {
            lv_obj_add_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
            b.active = false;
        } else {
            lv_opa_t opa = (lv_opa_t)(255 - (age * 255 / BLIP_LIFE_MS));
            lv_obj_set_style_bg_opa(b.obj, opa, 0);
        }
    }
}
