#include "flight_radar_app.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "settings.h"
#include "board.h"
#include "app_manager.h"
#include "airports_data.h"

#define TAG "FlightRadar"

// --- layout: 410 x 502 portrait ---
#define SCR_W   410
#define SCR_H   502
#define CX      205
#define CY      215
#define RADIUS  180

#define COL_BG     0x0A0E14
#define COL_PANEL  0x111823
#define COL_LINE   0x1E2A3A
#define COL_GREEN  0x16D3A2
#define COL_AMBER  0xFFB347
#define COL_RED    0xFF4D5E
#define COL_TEXT   0xC9D6E5
#define COL_MUTED  0x5F7086
#define COL_WHITE  0xFFFFFF

#define NVS_NS     "flightradar"
#define LAT_SCALE  100000.0f       // Settings เก็บได้แค่ int → สเกล 1e5 (~1 เมตร)

static FlightRadarApp* s_flight_radar = nullptr;

FlightRadarApp& FlightRadarApp::GetInstance() {
    if (s_flight_radar == nullptr) s_flight_radar = new FlightRadarApp();
    return *s_flight_radar;
}

void FlightRadarApp::DestroyInstance() {
    delete s_flight_radar;
    s_flight_radar = nullptr;
}

FlightRadarApp::~FlightRadarApp() {
    running_ = false;
    for (int i = 0; i < 60 && task_ != nullptr; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (display_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (timer_)  { lv_timer_delete(timer_); timer_ = nullptr; }
        if (screen_) { lv_obj_delete(screen_);  screen_ = nullptr; }
    }
    if (json_buf_) { heap_caps_free(json_buf_); json_buf_ = nullptr; }
}

// ---------------- พิกัดบ้าน (NVS) ----------------
void FlightRadarApp::LoadHome() {
    Settings s(NVS_NS, false);
    int32_t la = s.GetInt("lat", 0);
    int32_t lo = s.GetInt("lon", 0);
    range_nm_  = s.GetInt("range", FR_DEFAULT_RANGE);
    std::string city = s.GetString("city", "");
    snprintf(home_city_, sizeof(home_city_), "%s", city.c_str());
    if (la != 0 || lo != 0) {
        home_lat_ = (float)la / LAT_SCALE;
        home_lon_ = (float)lo / LAT_SCALE;
        has_home_ = true;
        // มีพิกัดแล้วแต่ยังไม่เคยเก็บชื่อเมือง (เช่นอัปเกรดมาจากเวอร์ชันก่อน)
        // → ค่อยดึงชื่อเมืองเพิ่มตอนเปิดแอป โดยไม่แตะพิกัดเดิม
        city_pending_ = (home_city_[0] == '\0');
    }
}

bool FlightRadarApp::SetHome(float lat, float lon) {
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
        ESP_LOGE(TAG, "พิกัดไม่ถูกต้อง: %d.%04d / %d.%04d",
                 (int)lat, (int)fabsf((lat - (int)lat) * 10000),
                 (int)lon, (int)fabsf((lon - (int)lon) * 10000));
        return false;
    }
    Settings s(NVS_NS, true);
    s.SetInt("lat", (int32_t)(lat * LAT_SCALE));
    s.SetInt("lon", (int32_t)(lon * LAT_SCALE));
    home_lat_ = lat; home_lon_ = lon; has_home_ = true;
    ESP_LOGI(TAG, "ตั้งพิกัดบ้านแล้ว");
    UpdateHomeLabels();
    return true;
}

void FlightRadarApp::SetRangeNm(int nm) {
    if (nm < 5)   nm = 5;
    if (nm > 250) nm = 250;          // ข้อจำกัดของ API
    range_nm_ = nm;
    Settings s(NVS_NS, true);
    s.SetInt("range", nm);
}

// ---------------- HTTP helper ----------------
// อ่าน response ทั้งก้อนลง buf (PSRAM) — คืนจำนวนไบต์ หรือ -1
static int HttpGet(const char* url, char* buf, int buf_size) {
    esp_http_client_config_t cfg = {};
    cfg.url             = url;
    cfg.timeout_ms      = 8000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // HTTPS กับ public CA
    cfg.user_agent      = "TARS-FlightRadar/1.0 (hobby; esp32)";

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    int total = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            total = 0;
            while (total < buf_size - 1) {
                int r = esp_http_client_read(c, buf + total, buf_size - 1 - total);
                if (r <= 0) break;
                total += r;
            }
            buf[total] = '\0';
        } else {
            ESP_LOGW(TAG, "HTTP %d จาก %s", status, url);
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return total;
}

// ---------------- IP geolocation (เรียกตอนเปิดแอปเท่านั้น) ----------------
bool FlightRadarApp::LocateByIp() {
    if (!json_buf_) return false;
    ESP_LOGI(TAG, "ขอพิกัดจาก IP...");
    int n = HttpGet("https://ipapi.co/json/", json_buf_, 4096);
    if (n <= 0) { ESP_LOGW(TAG, "IP geolocation ล้มเหลว"); return false; }

    cJSON* j = cJSON_Parse(json_buf_);
    if (!j) return false;
    cJSON* la = cJSON_GetObjectItem(j, "latitude");
    cJSON* lo = cJSON_GetObjectItem(j, "longitude");
    cJSON* ct = cJSON_GetObjectItem(j, "city");
    cJSON* rg = cJSON_GetObjectItem(j, "region");
    bool ok = false;
    if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
        ok = SetHome((float)la->valuedouble, (float)lo->valuedouble);
        if (ok) {
            const char* city = cJSON_IsString(ct) && ct->valuestring ? ct->valuestring
                             : (cJSON_IsString(rg) && rg->valuestring ? rg->valuestring : "");
            snprintf(home_city_, sizeof(home_city_), "%s", city);
            Settings st(NVS_NS, true);
            st.SetString("city", home_city_);
            UpdateHomeLabels();
        }
    }
    cJSON_Delete(j);
    return ok;
}


// ---------------- ป้ายบอกตำแหน่ง (city + lat/lon) ----------------
// nano printf ไม่รองรับ %f → ประกอบเลขเอง และต้องระวังค่าลบ
// (int)(-100.5) = -100 แล้วเศษจะติดลบซ้ำ ถ้าไม่ใช้ fabsf
static void FormatCoord(char* out, int out_sz, float lat, float lon) {
    int la_i = (int)lat,  lo_i = (int)lon;
    int la_f = (int)(fabsf(lat - (float)la_i) * 100.0f + 0.5f);
    int lo_f = (int)(fabsf(lon - (float)lo_i) * 100.0f + 0.5f);
    const char* la_sign = (lat < 0 && la_i == 0) ? "-" : "";   // กรณี -0.xx
    const char* lo_sign = (lon < 0 && lo_i == 0) ? "-" : "";
    snprintf(out, out_sz, "%s%d.%02d, %s%d.%02d",
             la_sign, la_i, la_f, lo_sign, lo_i, lo_f);
}

void FlightRadarApp::UpdateHomeLabels() {
    if (!built_) return;
    if (city_label_) {
        lv_label_set_text(city_label_, home_city_[0] ? home_city_ : "-");
    }
    if (coord_label_) {
        if (has_home_) {
            char c[32];
            FormatCoord(c, sizeof(c), home_lat_, home_lon_);
            lv_label_set_text(coord_label_, c);
        } else {
            lv_label_set_text(coord_label_, "no fix");
        }
    }
    ProjectAirports();
}

// ---------------- สนามบิน ----------------
// ข้อมูลอยู่ใน flash (static const) — สแกนทั้ง 4566 รายการถูกมาก เพราะเป็น int16
// และทำแค่ตอนพิกัด/range เปลี่ยน ไม่ใช่ทุกรอบ poll
void FlightRadarApp::ProjectAirports() {
    if (!built_ || !has_home_) return;

    const float range_km = range_nm_ * 1.852f;
    const float cos_lat  = cosf(home_lat_ * (float)M_PI / 180.0f);
    int n = 0;

    for (int i = 0; i < AIRPORT_NUM && n < FR_MAX_AIRPORTS; i++) {
        float alat = (float)AIRPORT_LAT[i] / AIRPORT_SCALE;
        float alon = (float)AIRPORT_LON[i] / AIRPORT_SCALE;
        float dx_km = (alon - home_lon_) * 111.32f * cos_lat;
        float dy_km = (alat - home_lat_) * 110.57f;
        if (fabsf(dx_km) > range_km || fabsf(dy_km) > range_km) continue;  // คัดหยาบก่อน
        if (sqrtf(dx_km * dx_km + dy_km * dy_km) > range_km) continue;

        int px = CX + (int)(dx_km / range_km * RADIUS);
        int py = CY - (int)(dy_km / range_km * RADIUS);

        // สามเหลี่ยม ∆ ชี้ขึ้น — วาดด้วย lv_line 4 จุด (ปิดรูปโดยกลับมาที่ยอด)
        const int R_TRI = 6;
        ap_tri_[n][0].x = px;           ap_tri_[n][0].y = py - R_TRI;
        ap_tri_[n][1].x = px - R_TRI;   ap_tri_[n][1].y = py + R_TRI - 2;
        ap_tri_[n][2].x = px + R_TRI;   ap_tri_[n][2].y = py + R_TRI - 2;
        ap_tri_[n][3].x = px;           ap_tri_[n][3].y = py - R_TRI;

        lv_obj_t* m = ap_mark_[n];
        lv_line_set_points(m, ap_tri_[n], 4);
        lv_obj_remove_flag(m, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* l = ap_lbl_[n];
        lv_label_set_text(l, AIRPORT_ICAO[i]);
        lv_obj_set_pos(l, px + R_TRI + 3, py - 6);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
        n++;
    }
    for (int i = n; i < FR_MAX_AIRPORTS; i++) {
        lv_obj_add_flag(ap_mark_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ap_lbl_[i],  LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "สนามบินในรัศมี: %d", n);
}

// ดึงเฉพาะชื่อเมือง — ไม่แตะพิกัด (กันกรณีผู้ใช้ตั้ง set_home เองไว้แล้ว)
bool FlightRadarApp::FetchCityOnly() {
    if (!json_buf_) return false;
    int n = HttpGet("https://ipapi.co/json/", json_buf_, 4096);
    if (n <= 0) return false;
    cJSON* j = cJSON_Parse(json_buf_);
    if (!j) return false;
    cJSON* ct = cJSON_GetObjectItem(j, "city");
    cJSON* rg = cJSON_GetObjectItem(j, "region");
    bool ok = false;
    const char* city = cJSON_IsString(ct) && ct->valuestring ? ct->valuestring
                     : (cJSON_IsString(rg) && rg->valuestring ? rg->valuestring : nullptr);
    if (city && city[0]) {
        snprintf(home_city_, sizeof(home_city_), "%s", city);
        Settings st(NVS_NS, true);
        st.SetString("city", home_city_);
        UpdateHomeLabels();
        ok = true;
        ESP_LOGI(TAG, "ได้ชื่อเมืองแล้ว");
    }
    cJSON_Delete(j);
    return ok;
}

// ---------------- ดึงข้อมูล + วาด ----------------
bool FlightRadarApp::FetchAndRender() {
    if (!has_home_ || !json_buf_) return false;

    // สร้าง URL — nano printf ไม่รองรับ %f จึงต้องประกอบเลขเอง
    int la_i = (int)home_lat_;
    int la_f = (int)fabsf((home_lat_ - la_i) * 10000.0f);
    int lo_i = (int)home_lon_;
    int lo_f = (int)fabsf((home_lon_ - lo_i) * 10000.0f);
    char url[160];
    snprintf(url, sizeof(url),
             "https://api.airplanes.live/v2/point/%d.%04d/%d.%04d/%d",
             la_i, la_f, lo_i, lo_f, range_nm_);

    int n = HttpGet(url, json_buf_, FR_JSON_BUF);
    if (n <= 0) {
        // fallback ตามที่ capsule-radar แนะนำ
        snprintf(url, sizeof(url),
                 "https://api.adsb.lol/v2/point/%d.%04d/%d.%04d/%d",
                 la_i, la_f, lo_i, lo_f, range_nm_);
        n = HttpGet(url, json_buf_, FR_JSON_BUF);
    }
    if (n <= 0) return false;

    RenderPlanes(json_buf_);
    return true;
}

void FlightRadarApp::RenderPlanes(const char* json) {
    cJSON* root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "parse JSON ไม่ได้"); return; }

    cJSON* ac = cJSON_GetObjectItem(root, "ac");
    if (!cJSON_IsArray(ac)) ac = cJSON_GetObjectItem(root, "aircraft");  // รองรับทั้ง 2 คีย์
    if (!cJSON_IsArray(ac)) { cJSON_Delete(root); return; }

    // ❗ฟังก์ชันนี้ถูกเรียกจาก PollTask ไม่ใช่ LVGL task
    // ทุกการแตะ LVGL ต้องอยู่ใน lock ไม่งั้นจอเพี้ยน/crash แบบสุ่ม
    DisplayLockGuard lock(display_);

    // ระยะ 1 ไมล์ทะเล = 1.852 กม.
    const float range_km = range_nm_ * 1.852f;
    const float cos_lat  = cosf(home_lat_ * (float)M_PI / 180.0f);

    int shown = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, ac) {
        if (shown >= FR_MAX_PLANES) break;
        cJSON* jla = cJSON_GetObjectItem(item, "lat");
        cJSON* jlo = cJSON_GetObjectItem(item, "lon");
        if (!cJSON_IsNumber(jla) || !cJSON_IsNumber(jlo)) continue;

        // equirectangular projection รอบจุดบ้าน (พอสำหรับรัศมี < 100 กม.)
        // ต้องคูณ cos(lat) ไม่งั้นแกน x จะเบี้ยว เพราะเส้นลองจิจูดแคบลงเมื่อห่างศูนย์สูตร
        float dx_km = ((float)jlo->valuedouble - home_lon_) * 111.32f * cos_lat;
        float dy_km = ((float)jla->valuedouble - home_lat_) * 110.57f;
        float dist  = sqrtf(dx_km * dx_km + dy_km * dy_km);
        if (dist > range_km) continue;                    // นอกวง — ข้าม

        int px = CX + (int)(dx_km / range_km * RADIUS);
        int py = CY - (int)(dy_km / range_km * RADIUS);   // ทิศเหนืออยู่บน

        // สีตามระดับความสูง
        uint32_t col = COL_GREEN;
        cJSON* alt = cJSON_GetObjectItem(item, "alt_baro");
        if (cJSON_IsNumber(alt)) {
            if (alt->valuedouble < 10000)      col = COL_RED;     // ต่ำ/กำลังขึ้นลง
            else if (alt->valuedouble < 25000) col = COL_AMBER;
        }

        lv_obj_t* dot = plane_dot_[shown];
        lv_obj_set_pos(dot, px - 4, py - 4);
        lv_obj_set_style_bg_color(dot, lv_color_hex(col), 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);

        // callsign
        cJSON* fl = cJSON_GetObjectItem(item, "flight");
        lv_obj_t* lbl = plane_lbl_[shown];
        if (cJSON_IsString(fl) && fl->valuestring) {
            char cs[10];
            snprintf(cs, sizeof(cs), "%.8s", fl->valuestring);
            for (int i = 0; cs[i]; i++) if (cs[i] == ' ') { cs[i] = '\0'; break; }
            lv_label_set_text(lbl, cs);
            lv_obj_set_style_text_color(lbl, lv_color_hex(col), 0);
            lv_obj_set_pos(lbl, px + 6, py - 6);
            lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
        shown++;
    }

    // ซ่อนที่เหลือ
    for (int i = shown; i < FR_MAX_PLANES; i++) {
        lv_obj_add_flag(plane_dot_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(plane_lbl_[i], LV_OBJ_FLAG_HIDDEN);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%d ac  %d nm", shown, range_nm_);
    lv_label_set_text(count_label_, buf);
    SetStatus("LIVE", COL_GREEN);

    cJSON_Delete(root);
}

// ---------------- UI ----------------
static void StripObj2(lv_obj_t* o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
}

void FlightRadarApp::SetStatus(const char* text, uint32_t color) {
    if (!status_label_) return;
    lv_label_set_text(status_label_, text);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(color), 0);
}

void FlightRadarApp::BuildUi() {
    if (built_) return;

    screen_ = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen_, 0, 0);

    lv_obj_t* title = lv_label_create(screen_);
    lv_label_set_text(title, "FLIGHT RADAR");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // วง radar ที่ใช้ร่วมกับแอปอื่นได้
    scope_.Build(screen_, CX, CY, RADIUS, SCR_W, SCR_H, COL_GREEN);

    // จุดกลาง = ตำแหน่งเรา
    lv_obj_t* home = lv_obj_create(screen_);
    StripObj2(home);
    lv_obj_set_size(home, 7, 7);
    lv_obj_set_pos(home, CX - 3, CY - 3);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);

    // pool สนามบิน (สร้างก่อน → อยู่ชั้นล่างกว่าเครื่องบิน)
    for (int i = 0; i < FR_MAX_AIRPORTS; i++) {
        // สามเหลี่ยม ∆ สีขาว — lv_line กินพื้นที่เต็มจอแล้วใช้พิกัดสัมบูรณ์
        lv_obj_t* m = lv_line_create(screen_);
        StripObj2(m);
        lv_obj_set_size(m, SCR_W, SCR_H);
        lv_obj_set_pos(m, 0, 0);
        lv_obj_set_style_line_width(m, 1, 0);
        lv_obj_set_style_line_color(m, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_line_opa(m, LV_OPA_COVER, 0);
        lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
        ap_mark_[i] = m;

        lv_obj_t* l = lv_label_create(screen_);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_WHITE), 0);
        lv_label_set_text(l, "");
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        ap_lbl_[i] = l;
    }

    // pool เครื่องบิน
    for (int i = 0; i < FR_MAX_PLANES; i++) {
        lv_obj_t* d = lv_obj_create(screen_);
        StripObj2(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(COL_GREEN), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        plane_dot_[i] = d;

        lv_obj_t* l = lv_label_create(screen_);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_GREEN), 0);
        lv_label_set_text(l, "");
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        plane_lbl_[i] = l;
    }

    // แถบล่าง
    lv_obj_t* bar = lv_obj_create(screen_);
    StripObj2(bar);
    lv_obj_set_size(bar, SCR_W - 32, 56);
    lv_obj_set_pos(bar, 16, 420);
    lv_obj_set_style_radius(bar, 12, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(COL_LINE), 0);

    // ซ้ายบน: สถานะ | ขวาบน: เมือง
    status_label_ = lv_label_create(bar);
    lv_label_set_text(status_label_, "STARTING");
    lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(status_label_, LV_ALIGN_LEFT_MID, 12, -12);

    city_label_ = lv_label_create(bar);
    lv_label_set_text(city_label_, "-");
    lv_obj_set_style_text_color(city_label_, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(city_label_, LV_ALIGN_RIGHT_MID, -12, -12);

    // ซ้ายล่าง: จำนวน+รัศมี | ขวาล่าง: พิกัด
    count_label_ = lv_label_create(bar);
    lv_label_set_text(count_label_, "-");
    lv_obj_set_style_text_color(count_label_, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(count_label_, LV_ALIGN_LEFT_MID, 12, 12);

    coord_label_ = lv_label_create(bar);
    lv_label_set_text(coord_label_, "no fix");
    lv_obj_set_style_text_color(coord_label_, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(coord_label_, LV_ALIGN_RIGHT_MID, -12, 12);

    built_ = true;
    UpdateHomeLabels();          // แสดงค่าที่โหลดจาก NVS ทันที
}

// ---------------- lifecycle ----------------
void FlightRadarApp::TimerCb(lv_timer_t* t) {
    auto* self = static_cast<FlightRadarApp*>(lv_timer_get_user_data(t));
    self->scope_.TickSweep(0.05f);

    // กันจอดับระหว่างใช้แอป — SetPowerSaveLevel(PERFORMANCE) เรียก WakeUp()
    // ซึ่งรีเซ็ต ticks_ ของ PowerSaveTimer เป็น 0 (ตัวนับ 30 วิจึงไม่ครบสักที)
    // ทำทุก ~10 วิ (timer 40ms x 250) พอ ไม่ต้องแก้ไฟล์ board
    if (++self->keepawake_tick_ >= 250) {
        self->keepawake_tick_ = 0;
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }
}

void FlightRadarApp::PollTask(void* arg) {
    auto* self = static_cast<FlightRadarApp*>(arg);

    // ครั้งแรกที่เปิดแอปและยังไม่เคยตั้งพิกัด → หาจาก IP ให้เลย
    if (!self->has_home_) {
        {
            DisplayLockGuard lock(self->display_);
            self->SetStatus("LOCATING", COL_AMBER);
        }
        if (!self->LocateByIp()) {
            DisplayLockGuard lock(self->display_);
            self->SetStatus("SET HOME", COL_RED);
            lv_label_set_text(self->count_label_, "no location");
            self->running_ = false;
            self->task_ = nullptr;
            vTaskDelete(NULL);
            return;
        }
    }

    // มีพิกัดแล้วแต่ยังไม่รู้ชื่อเมือง → ดึงเพิ่มครั้งเดียว ไม่แตะพิกัด
    if (self->city_pending_) {
        self->city_pending_ = false;
        self->FetchCityOnly();
    }

    while (self->running_) {
        {
            DisplayLockGuard lock(self->display_);
            self->SetStatus("FETCHING", COL_AMBER);
        }
        bool ok;
        {
            // ดึงข้อมูลนอก display lock (ใช้เวลานาน) แล้วค่อยล็อกตอนวาด
            ok = self->FetchAndRender();
        }
        if (!ok) {
            DisplayLockGuard lock(self->display_);
            self->SetStatus("NO DATA", COL_RED);
        }
        for (int i = 0; i < FR_POLL_MS / 100 && self->running_; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    self->task_ = nullptr;
    ESP_LOGI(TAG, "poll task exit");
    vTaskDelete(NULL);
}

void FlightRadarApp::Show() {
    display_ = Board::GetInstance().GetDisplay();
    DisplayLockGuard lock(display_);
    if (visible_) return;

    if (!json_buf_) {
        // JSON จาก ADS-B อาจใหญ่หลายสิบ KB → ต้องอยู่ใน PSRAM
        json_buf_ = (char*)heap_caps_malloc(FR_JSON_BUF, MALLOC_CAP_SPIRAM);
        if (!json_buf_) {
            ESP_LOGE(TAG, "alloc PSRAM %d ไม่ได้", FR_JSON_BUF);
            return;
        }
    }
    LoadHome();
    BuildUi();

    prev_screen_ = AppManager::ReturnScreen();
    lv_screen_load_anim(screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, 300, 0, false);
    if (!timer_) timer_ = lv_timer_create(TimerCb, 40, this);
    lv_timer_resume(timer_);

    running_ = true;
    xTaskCreate(PollTask, "fr_poll", 8192, this, 3, &task_);
    visible_ = true;
    ESP_LOGI(TAG, "shown (range %d nm, home %s)", range_nm_, has_home_ ? "ok" : "ยังไม่ตั้ง");
}

void FlightRadarApp::Hide() {
    running_ = false;
    for (int i = 0; i < 60 && task_ != nullptr; i++) vTaskDelay(pdMS_TO_TICKS(50));

    DisplayLockGuard lock(display_);
    if (!visible_) return;
    if (timer_) lv_timer_pause(timer_);
    if (prev_screen_) {
        lv_screen_load_anim(prev_screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, 300, 0, false);
    }
    visible_ = false;
    // คืนการจัดการ power save ให้ระบบ (จอจะดับเองตามปกติหลัง 30 วิ)
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::BALANCED);
    ESP_LOGI(TAG, "hidden");
}
