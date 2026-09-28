/*
 * RadarScope — วง radar ที่ใช้ซ้ำได้ (rings + spokes + เส้นกวาด)
 *
 * แยกออกมาเพื่อให้ mini app หลายตัวใช้หน้าตาเดียวกัน:
 *   - RadarApp        (WiFi CSI — ตรวจคนในห้อง)
 *   - FlightRadarApp  (ADS-B — เครื่องบินรอบตัว)
 *
 * วาดด้วย LVGL widgets ไม่ใช่ lv_canvas — canvas เต็มจอ 410x502 RGB565
 * กิน ~411 KB ซึ่งไม่คุ้มเลยสำหรับรูปทรงง่าย ๆ แบบนี้
 */
#ifndef RADAR_SCOPE_H
#define RADAR_SCOPE_H

#include <lvgl.h>

#define SCOPE_SPOKES  12

class RadarScope {
public:
    // สร้าง scope บน parent (ปกติคือ screen) ที่จุดศูนย์กลาง cx,cy รัศมี r
    void Build(lv_obj_t* parent, int cx, int cy, int r,
               int screen_w, int screen_h, uint32_t color);

    // หมุนเส้นกวาดไปข้างหน้า — เรียกจาก lv_timer
    void TickSweep(float step_rad = 0.09f);

    lv_obj_t* SweepLine() const { return sweep_; }
    int CenterX() const { return cx_; }
    int CenterY() const { return cy_; }
    int Radius()  const { return r_; }

private:
    lv_obj_t* sweep_ = nullptr;
    int cx_ = 0, cy_ = 0, r_ = 0;
    float angle_ = 0.0f;

    // LVGL เก็บเป็น pointer → array ต้องอยู่ยาว (เป็น member ห้ามเป็น local)
    lv_point_precise_t sweep_pts_[2];
    lv_point_precise_t spoke_pts_[SCOPE_SPOKES][2];
};

#endif // RADAR_SCOPE_H
