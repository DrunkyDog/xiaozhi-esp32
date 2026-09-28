#include "radar_scope.h"
#include <math.h>

// ล้าง style ตั้งต้นของ lv_obj ให้เป็นกล่องใส
static void StripObj(lv_obj_t* o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
}

void RadarScope::Build(lv_obj_t* parent, int cx, int cy, int r,
                       int screen_w, int screen_h, uint32_t color) {
    cx_ = cx; cy_ = cy; r_ = r;

    // rings 4 ชั้น
    for (int i = 1; i <= 4; i++) {
        int d = (r * 2) * i / 4;
        lv_obj_t* ring = lv_obj_create(parent);
        StripObj(ring);
        lv_obj_set_size(ring, d, d);
        lv_obj_set_pos(ring, cx - d / 2, cy - d / 2);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_20, 0);
    }

    // spokes ทุก 30°
    for (int i = 0; i < SCOPE_SPOKES; i++) {
        float a = (float)i * (2.0f * (float)M_PI / SCOPE_SPOKES);
        spoke_pts_[i][0].x = cx;
        spoke_pts_[i][0].y = cy;
        spoke_pts_[i][1].x = (lv_value_precise_t)(cx + r * cosf(a));
        spoke_pts_[i][1].y = (lv_value_precise_t)(cy + r * sinf(a));

        lv_obj_t* spoke = lv_line_create(parent);
        StripObj(spoke);
        lv_obj_set_size(spoke, screen_w, screen_h);
        lv_obj_set_pos(spoke, 0, 0);
        lv_line_set_points(spoke, spoke_pts_[i], 2);
        lv_obj_set_style_line_width(spoke, 1, 0);
        lv_obj_set_style_line_color(spoke, lv_color_hex(color), 0);
        lv_obj_set_style_line_opa(spoke, LV_OPA_20, 0);
    }

    // เส้นกวาด
    sweep_pts_[0].x = cx;      sweep_pts_[0].y = cy;
    sweep_pts_[1].x = cx + r;  sweep_pts_[1].y = cy;
    sweep_ = lv_line_create(parent);
    StripObj(sweep_);
    lv_obj_set_size(sweep_, screen_w, screen_h);
    lv_obj_set_pos(sweep_, 0, 0);
    lv_line_set_points(sweep_, sweep_pts_, 2);
    lv_obj_set_style_line_width(sweep_, 2, 0);
    lv_obj_set_style_line_color(sweep_, lv_color_hex(color), 0);
    lv_obj_set_style_line_opa(sweep_, LV_OPA_COVER, 0);
}

void RadarScope::TickSweep(float step_rad) {
    if (!sweep_) return;
    angle_ += step_rad;
    if (angle_ > 2.0f * (float)M_PI) angle_ -= 2.0f * (float)M_PI;
    sweep_pts_[1].x = (lv_value_precise_t)(cx_ + r_ * cosf(angle_));
    sweep_pts_[1].y = (lv_value_precise_t)(cy_ + r_ * sinf(angle_));
    lv_line_set_points(sweep_, sweep_pts_, 2);
}
