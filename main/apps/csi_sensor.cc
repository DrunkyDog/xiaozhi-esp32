#include "csi_sensor.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_csi_gain_ctrl.h>
#include <ping/ping_sock.h>
#include <math.h>
#include <string.h>

#define TAG "CsiSensor"

// ค่าเดียวกับที่จูนไว้ฝั่ง Python (backend/processing.py)
#define THRESHOLD      3.0f
#define ENTER_FRAMES   3
#define EXIT_FRAMES    4

static esp_ping_handle_t s_ping = nullptr;

CsiSensor& CsiSensor::GetInstance() {
    static CsiSensor inst;
    return inst;
}

// ---------------- CSI callback (WiFi task — ต้องเร็วที่สุด) ----------------
void CsiSensor::CsiRxCb(void* ctx, wifi_csi_info_t* info) {
    CsiSensor* self = static_cast<CsiSensor*>(ctx);
    if (!info || !info->buf || !self->running_) return;
    if (memcmp(info->mac, self->bssid_, 6) != 0) return;      // เอาเฉพาะ packet จาก router

    // ชดเชย AGC/FFT gain — ถ้าไม่ทำ amplitude จะกระโดดเองเวลาชิปปรับ gain
    static int   s_count = 0;
    static uint8_t agc = 0;
    static int8_t  fft = 0;
    float gain = 1.0f;
    esp_csi_gain_ctrl_get_rx_gain(&info->rx_ctrl, &agc, &fft);
    if (s_count < 100) {
        esp_csi_gain_ctrl_record_rx_gain(agc, fft);
        s_count++;
    }
    esp_csi_gain_ctrl_get_gain_compensation(&gain, agc, fft);

    RawFrame f;
    f.len  = info->len > (int)sizeof(f.buf) ? sizeof(f.buf) : info->len;
    f.gain = gain;
    memcpy(f.buf, info->buf, f.len);
    xQueueSend(self->queue_, &f, 0);        // ทิ้ง frame ถ้า queue เต็ม ดีกว่าบล็อก WiFi
}

// ---------------- processing task ----------------
void CsiSensor::ProcTask(void* arg) {
    CsiSensor* self = static_cast<CsiSensor*>(arg);
    RawFrame f;
    while (self->running_) {
        if (xQueueReceive(self->queue_, &f, pdMS_TO_TICKS(200)) == pdTRUE) {
            self->Process(f);
        }
    }
    self->task_ = nullptr;
    ESP_LOGI(TAG, "proc task exit");
    vTaskDelete(NULL);
}

void CsiSensor::Process(const RawFrame& f) {
    int n = f.len / 2;
    if (n > CSI_MAX_SUBCARRIERS) n = CSI_MAX_SUBCARRIERS;
    if (n <= 0) return;
    n_sub_ = n;

    // amplitude ต่อ subcarrier: buf = [imag0, real0, imag1, real1, ...]
    float amp[CSI_MAX_SUBCARRIERS];
    for (int k = 0; k < n; k++) {
        float im = f.gain * f.buf[2 * k];
        float re = f.gain * f.buf[2 * k + 1];
        amp[k] = sqrtf(re * re + im * im);
    }

    // --- ช่วง calibrate: สะสม mean/std ของห้องว่าง ---
    if (!calibrated_) {
        for (int k = 0; k < n; k++) {
            calib_sum_[k] += amp[k];
            calib_sq_[k]  += amp[k] * amp[k];
        }
        if (++calib_n_ >= CSI_CALIB_FRAMES) {
            for (int k = 0; k < n; k++) {
                float m = calib_sum_[k] / calib_n_;
                float v = calib_sq_[k] / calib_n_ - m * m;
                base_mean_[k] = m;
                base_std_[k]  = (v > 0 ? sqrtf(v) : 0.0f) + 1e-6f;   // กันหาร 0
            }
            calibrated_ = true;
            ESP_LOGI(TAG, "calibrated: %d subcarriers, %d frames", n, calib_n_);
        }
        if (cb_) cb_(0.0f, false, true);
        return;
    }

    // --- push เข้า sliding window ---
    memcpy(window_[win_head_], amp, n * sizeof(float));
    win_head_ = (win_head_ + 1) % CSI_WINDOW;
    if (win_count_ < CSI_WINDOW) win_count_++;

    float score = MotionScore();
    bool  present = DetectPresence(score);
    if (cb_) cb_(score, present, false);
}

// std ตามเวลาต่อ subcarrier / baseline_std → เฉลี่ยข้าม subcarrier
float CsiSensor::MotionScore() {
    if (win_count_ < 4) return 0.0f;
    float total = 0.0f;
    for (int k = 0; k < n_sub_; k++) {
        float sum = 0.0f, sq = 0.0f;
        for (int t = 0; t < win_count_; t++) {
            float v = window_[t][k];
            sum += v; sq += v * v;
        }
        float m = sum / win_count_;
        float var = sq / win_count_ - m * m;
        float sd = var > 0 ? sqrtf(var) : 0.0f;
        total += sd / base_std_[k];
    }
    return total / n_sub_;
}

// threshold + hysteresis — ตรงกับ Python detect_presence ทุกบรรทัด
bool CsiSensor::DetectPresence(float score) {
    if (!present_) {
        if (score >= THRESHOLD) {
            if (++streak_ >= ENTER_FRAMES) { present_ = true; streak_ = 0; }
        } else {
            streak_ = 0;
        }
    } else {
        if (score < THRESHOLD) {
            if (++streak_ >= EXIT_FRAMES) { present_ = false; streak_ = 0; }
        } else {
            streak_ = 0;
        }
    }
    return present_;
}

// ---------------- lifecycle ----------------
bool CsiSensor::Start(Callback cb) {
    if (running_) return true;
    cb_ = cb;

    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ESP_LOGE(TAG, "ยังไม่ได้ต่อ WiFi — เริ่ม CSI ไม่ได้");
        return false;
    }
    memcpy(bssid_, ap.bssid, 6);

    if (!queue_) queue_ = xQueueCreate(CSI_QUEUE_LEN, sizeof(RawFrame));
    if (!queue_) { ESP_LOGE(TAG, "สร้าง queue ไม่ได้"); return false; }

    Recalibrate();
    running_ = true;

    // config สำหรับ ESP32/S3: LLTF อย่างเดียว = 64 subcarrier
    wifi_csi_config_t cfg = {};
    cfg.lltf_en           = true;
    cfg.htltf_en          = false;
    cfg.stbc_htltf2_en    = false;
    cfg.ltf_merge_en      = true;
    cfg.channel_filter_en = true;
    cfg.manu_scale        = true;
    cfg.shift             = true;

    // ❗ห้ามใช้ ESP_ERROR_CHECK ที่นี่ — ถ้า CSI พังต้องไม่ลาก voice assistant reboot ไปด้วย
    esp_err_t err = esp_wifi_set_csi_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_csi_config ล้มเหลว: %s "
                 "(ถ้าเป็น ESP_ERR_NOT_SUPPORTED ให้เปิด CONFIG_ESP_WIFI_CSI_ENABLED=y)",
                 esp_err_to_name(err));
        running_ = false;
        return false;
    }
    if ((err = esp_wifi_set_csi_rx_cb(CsiRxCb, this)) != ESP_OK) {
        ESP_LOGE(TAG, "set_csi_rx_cb ล้มเหลว: %s", esp_err_to_name(err));
        running_ = false;
        return false;
    }
    if ((err = esp_wifi_set_csi(true)) != ESP_OK) {
        ESP_LOGE(TAG, "set_csi ล้มเหลว: %s", esp_err_to_name(err));
        running_ = false;
        return false;
    }

    // CSI ต้องการวิทยุตื่นตลอด — power save ทำให้ packet หายเป็นช่วง
    esp_wifi_set_ps(WIFI_PS_NONE);

    xTaskCreate(ProcTask, "csi_proc", 4096, this, 4, &task_);

    // ping gateway ให้มี traffic สม่ำเสมอ → CSI ไหลนิ่ง
    if (!s_ping) {
        esp_ping_config_t p = ESP_PING_DEFAULT_CONFIG();
        p.count           = 0;                    // ไม่จำกัด
        p.interval_ms     = 1000 / CSI_PING_HZ;
        p.task_stack_size = 3072;
        p.data_size       = 1;

        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
        p.target_addr.u_addr.ip4.addr = ip.gw.addr;
        p.target_addr.type = ESP_IPADDR_TYPE_V4;

        esp_ping_callbacks_t cbs = {};
        if (esp_ping_new_session(&p, &cbs, &s_ping) == ESP_OK) {
            esp_ping_start(s_ping);
        }
    } else {
        esp_ping_start(s_ping);
    }

    ESP_LOGI(TAG, "started — calibrating %d frames (~%d วิ)",
             CSI_CALIB_FRAMES, CSI_CALIB_FRAMES / CSI_PING_HZ);
    return true;
}

void CsiSensor::Stop() {
    if (!running_) return;
    running_ = false;
    esp_wifi_set_csi(false);
    if (s_ping) esp_ping_stop(s_ping);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);      // คืน power save ให้ระบบ
    ESP_LOGI(TAG, "stopped");
}

void CsiSensor::Recalibrate() {
    memset(calib_sum_, 0, sizeof(calib_sum_));
    memset(calib_sq_,  0, sizeof(calib_sq_));
    calib_n_ = 0;
    calibrated_ = false;
    win_count_ = 0;
    win_head_  = 0;
    present_   = false;
    streak_    = 0;
}
