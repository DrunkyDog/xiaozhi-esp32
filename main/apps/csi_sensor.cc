#include "csi_sensor.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_csi_gain_ctrl.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include "sd_card.h"
#include "settings.h"
#include <ping/ping_sock.h>
#include <math.h>
#include <string.h>
#include <string>

#define TAG "CsiSensor"

#define ENTER_FRAMES   3        // เกิน high กี่ frame ติดกันถึงประกาศว่ามีคน
#define NVS_NS         "csiradar"

static esp_ping_handle_t s_ping = nullptr;

CsiSensor& CsiSensor::GetInstance() {
    static CsiSensor inst;
    return inst;
}

// ---------------- CSI callback (WiFi task — ต้องเร็วที่สุด) ----------------
void CsiSensor::CsiRxCb(void* ctx, wifi_csi_info_t* info) {
    CsiSensor* self = static_cast<CsiSensor*>(ctx);
    if (!info || !info->buf || !self->running_) return;

    // เลือกแหล่งสัญญาณ — ห้ามรับปนกัน เพราะ CSI จาก router กับจาก sender
    // เดินทางคนละเส้น ถ้าผสมจะดูเหมือนมีคนขยับตลอดเวลา
    int64_t now = esp_timer_get_time();
    bool from_sender = self->sender_set_ && memcmp(info->mac, self->sender_mac_, 6) == 0;
    bool from_router = memcmp(info->mac, self->bssid_, 6) == 0;
    if (from_sender) {
        self->last_sender_us_ = now;
    } else if (from_router) {
        // ใช้ router ต่อเมื่อไม่มี sender หรือ sender เงียบไปเกิน 3 วิ (ไฟดับ/ถอดปลั๊ก)
        if (self->sender_set_ && (now - self->last_sender_us_) < 3000000) return;
    } else {
        return;
    }

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
    // วัดอัตรา CSI จริง (ping 50Hz ไม่ได้แปลว่า callback มา 50Hz)
    // ค่านี้ต้องใช้ตอนคำนวณ BPM — ถ้าใช้ค่าสมมติ BPM จะผิดสเกล
    int64_t now_us = esp_timer_get_time();
    if (last_frame_us_ != 0) {
        float dt = (float)(now_us - last_frame_us_) / 1e6f;
        if (dt > 0.0005f && dt < 1.0f) {
            float inst = 1.0f / dt;
            csi_rate_hz_ = (csi_rate_hz_ == 0.0f) ? inst
                                                  : csi_rate_hz_ * 0.95f + inst * 0.05f;
        }
    }
    last_frame_us_ = now_us;

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
        LogSample(0.0f, false, true);
        if (cb_) cb_(0.0f, false, true);
        return;
    }

    // --- Welford online variance ต่อ subcarrier (ไม่ต้องเก็บ buffer ยาว) ---
    wf_n_++;
    for (int k = 0; k < n; k++) {
        float d  = amp[k] - wf_mean_[k];
        wf_mean_[k] += d / (float)wf_n_;
        wf_m2_[k]   += d * (amp[k] - wf_mean_[k]);
    }
    if (wf_n_ % 100 == 0) SelectTopK();      // อัปเดต top-K เป็นระยะ ไม่ต้องทุก frame

    // --- push เข้า sliding window ---
    memcpy(window_[win_head_], amp, n * sizeof(float));
    win_head_ = (win_head_ + 1) % CSI_WINDOW;
    if (win_count_ < CSI_WINDOW) win_count_++;

    float score = MotionScore();

    // --- stage 2: เรียน ambient เพื่อตั้ง threshold เอง ---
    if (!ambient_done_) {
        if (score > 0.0f) {
            amb_sum_ += score;
            amb_sq_  += (double)score * score;
            amb_n_++;
        }
        if (amb_n_ >= CSI_AMBIENT_FRAMES) {
            double m  = amb_sum_ / amb_n_;
            double v  = amb_sq_ / amb_n_ - m * m;
            double sd = v > 0 ? sqrt(v) : 0.0;
            thr_high_ = (float)(m + CSI_SIGMA_MULT * sd);
            thr_low_  = thr_high_ * CSI_HYST_RATIO;
            ambient_done_ = true;
            SaveThresholds();
            ESP_LOGI(TAG, "ambient เรียนเสร็จ: mean=%d.%02d high=%d.%02d low=%d.%02d rate=%dHz",
                     (int)m, (int)((m-(int)m)*100),
                     (int)thr_high_, (int)((thr_high_-(int)thr_high_)*100),
                     (int)thr_low_,  (int)((thr_low_-(int)thr_low_)*100),
                     (int)csi_rate_hz_);
        }
        LogSample(score, false, true);
        if (cb_) cb_(score, false, true);     // ยังถือว่า calibrating อยู่
        return;
    }

    // ป้อน subcarrier ที่ตอบสนองดีสุดเข้าเครื่องตรวจการหายใจ
    if (top_k_count_ > 0) FeedBreathing(amp[top_k_[0]]);

    bool  present = DetectPresence(score);
    LogSample(score, present, false);
    if (cb_) cb_(score, present, false);
}

// เลือก K subcarrier ที่ variance สูงสุด — ตัดตัวที่ตายหรือมีแต่ noise ทิ้ง
// (แนวคิดจาก RuView: EDGE_TOP_K 8)
void CsiSensor::SelectTopK() {
    if (wf_n_ < 2 || n_sub_ <= 0) return;
    int k = CSI_TOP_K > n_sub_ ? n_sub_ : CSI_TOP_K;
    bool used[CSI_MAX_SUBCARRIERS] = {};
    top_k_count_ = 0;
    for (int ki = 0; ki < k; ki++) {
        float best = -1.0f; int best_i = -1;
        for (int sc = 0; sc < n_sub_; sc++) {
            if (used[sc]) continue;
            float var = wf_m2_[sc] / (float)(wf_n_ - 1);
            if (var > best) { best = var; best_i = sc; }
        }
        if (best_i < 0) break;
        used[best_i] = true;
        top_k_[top_k_count_++] = (uint8_t)best_i;
    }
}

// std ตามเวลา หารด้วย baseline_std — เฉลี่ยเฉพาะ subcarrier ใน top-K
float CsiSensor::MotionScore() {
    if (win_count_ < 4) return 0.0f;
    // ยังไม่ได้เลือก top-K (ช่วงแรก) → ใช้ทุก subcarrier ไปก่อน
    int count = top_k_count_ > 0 ? top_k_count_ : n_sub_;
    float total = 0.0f;
    for (int idx = 0; idx < count; idx++) {
        int k = top_k_count_ > 0 ? top_k_[idx] : idx;
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
    return total / count;
}

// hysteresis 2 ระดับ: เข้าที่ thr_high_, ออกที่ thr_low_ (= 0.5 * high)
// ช่องว่างระหว่าง 2 ค่าทำให้ไม่กระพริบตอน score แกว่งรอบเกณฑ์
bool CsiSensor::DetectPresence(float score) {
    if (!present_) {
        if (score >= thr_high_) {
            if (++streak_ >= ENTER_FRAMES) { present_ = true; streak_ = 0; }
        } else {
            streak_ = 0;
        }
    } else {
        if (score < thr_low_) {
            if (++streak_ >= CSI_CLEAR_FRAMES) { present_ = false; streak_ = 0; }
        } else {
            streak_ = 0;
        }
    }
    return present_;
}


// ---------------- ตรวจอัตราการหายใจ ----------------
// biquad bandpass 2nd-order (สูตรเดียวกับ RuView / RBJ audio cookbook)
void CsiSensor::BiquadDesign(Biquad& bq, float fs, float f_lo, float f_hi) {
    float w0 = 2.0f * (float)M_PI * (f_lo + f_hi) / 2.0f / fs;
    float bw = 2.0f * (float)M_PI * (f_hi - f_lo) / fs;
    float alpha = sinf(w0) * sinhf(logf(2.0f) / 2.0f * bw / sinf(w0));
    float a0i = 1.0f / (1.0f + alpha);
    bq.b0 =  alpha * a0i;
    bq.b1 =  0.0f;
    bq.b2 = -alpha * a0i;
    bq.a1 = -2.0f * cosf(w0) * a0i;
    bq.a2 =  (1.0f - alpha) * a0i;
    bq.x1 = bq.x2 = bq.y1 = bq.y2 = 0.0f;
}

float CsiSensor::BiquadApply(Biquad& bq, float x) {
    float y = bq.b0 * x + bq.b1 * bq.x1 + bq.b2 * bq.x2
              - bq.a1 * bq.y1 - bq.a2 * bq.y2;
    bq.x2 = bq.x1; bq.x1 = x;
    bq.y2 = bq.y1; bq.y1 = y;
    return y;
}

// รับ amplitude ของ subcarrier หลัก → decimate → กรอง → เก็บประวัติ
void CsiSensor::FeedBreathing(float sample) {
    // ปรับอัตรา decimate ตามอัตรา CSI จริงที่วัดได้
    if (csi_rate_hz_ > 1.0f) {
        int t = (int)(csi_rate_hz_ / CSI_BREATH_FS + 0.5f);
        decim_target_ = t < 1 ? 1 : t;
    }
    decim_acc_ += sample;
    if (++decim_n_ < decim_target_) return;
    float avg = decim_acc_ / decim_n_;
    decim_acc_ = 0.0f; decim_n_ = 0;

    if (!bp_ready_) {
        BiquadDesign(bp_, CSI_BREATH_FS, CSI_BREATH_LO_HZ, CSI_BREATH_HI_HZ);
        bp_ready_ = true;
    }
    float y = BiquadApply(bp_, avg);
    breath_hist_[breath_idx_] = y;
    breath_idx_ = (breath_idx_ + 1) % CSI_BREATH_HIST;
    if (breath_len_ < CSI_BREATH_HIST) breath_len_++;

    // คำนวณ BPM ทุก ~1 วินาที (10 sample ที่ 10Hz)
    if (++bpm_tick_ >= 10) { bpm_tick_ = 0; breath_bpm_ = EstimateBpm(); }
}

// นับจุดตัดศูนย์ขาขึ้น → คาบเฉลี่ย → BPM
float CsiSensor::EstimateBpm() {
    if (breath_len_ < CSI_BREATH_HIST / 2) return 0.0f;   // ประวัติยังสั้นเกิน

    // คลี่ ring buffer เป็นลำดับเวลา (เก่า → ใหม่)
    float seq[CSI_BREATH_HIST];
    int start = (breath_idx_ - breath_len_ + CSI_BREATH_HIST) % CSI_BREATH_HIST;
    for (int i = 0; i < breath_len_; i++)
        seq[i] = breath_hist_[(start + i) % CSI_BREATH_HIST];

    int cross[64], nc = 0;
    for (int i = 1; i < breath_len_ && nc < 64; i++)
        if (seq[i-1] <= 0.0f && seq[i] > 0.0f) cross[nc++] = i;
    if (nc < 3) return 0.0f;              // ต้องมีอย่างน้อย 2 คาบเต็ม

    float total = 0.0f;
    for (int i = 1; i < nc; i++) total += (float)(cross[i] - cross[i-1]);
    float period = total / (float)(nc - 1);
    if (period < 1.0f) return 0.0f;

    float bpm = (CSI_BREATH_FS / period) * 60.0f;
    // ตัดค่าที่อยู่นอกช่วงการหายใจมนุษย์ — น่าจะเป็น noise
    if (bpm < 6.0f || bpm > 30.0f) return 0.0f;
    return bpm;
}

// ---------------- threshold ที่เรียนรู้แล้ว เก็บใน NVS ----------------
// เรียนครั้งเดียวใช้ตลอด — ไม่ต้องรอ 15 วิทุกครั้งที่เปิดแอป
void CsiSensor::LoadThresholds() {
    Settings st(NVS_NS, false);
    int32_t h = st.GetInt("thr_high", 0);
    if (h > 0) {
        thr_high_ = (float)h / 1000.0f;
        thr_low_  = thr_high_ * CSI_HYST_RATIO;
        ambient_done_ = true;
        ESP_LOGI(TAG, "ใช้ threshold ที่เรียนไว้: high=%d.%03d", h / 1000, h % 1000);
    }
}

void CsiSensor::SaveThresholds() {
    Settings st(NVS_NS, true);
    st.SetInt("thr_high", (int32_t)(thr_high_ * 1000.0f));
}

// ---------------- logging ลง SD ----------------
// ⚠️ ชื่อไฟล์ต้อง 8.3 (CONFIG_FATFS_LFN_NONE) → CSInnnnn.CSV
bool CsiSensor::StartLogging() {
    if (log_fp_) return true;
    if (!SdCard::GetInstance().Mount()) {
        ESP_LOGW(TAG, "ไม่มี SD — ข้ามการ log");
        return false;
    }
    // หาเลขไฟล์ถัดไปที่ยังว่าง
    for (int i = 1; i < 100000; i++) {
        snprintf(log_path_, sizeof(log_path_), SD_MOUNT_POINT "/CSI%05d.CSV", i);
        struct stat st;
        if (stat(log_path_, &st) != 0) break;       // ยังไม่มีไฟล์นี้ → ใช้เลยนี้
    }
    log_fp_ = fopen(log_path_, "w");
    if (!log_fp_) {
        ESP_LOGE(TAG, "เปิดไฟล์ %s ไม่ได้ (ชื่อต้องเป็น 8.3)", log_path_);
        return false;
    }
    fprintf(log_fp_, "ms,score,present,calibrating\n");
    log_len_ = 0;
    ESP_LOGI(TAG, "logging → %s", log_path_);
    return true;
}

void CsiSensor::StopLogging() {
    if (!log_fp_) return;
    if (log_len_ > 0) { fwrite(log_buf_, 1, log_len_, log_fp_); log_len_ = 0; }
    fclose(log_fp_);
    log_fp_ = nullptr;
    ESP_LOGI(TAG, "logging หยุด → %s", log_path_);
}

void CsiSensor::LogSample(float score, bool present, bool calibrating) {
    if (!log_fp_) return;
    // ⚠️ nano printf ไม่รองรับ %f → แยกส่วนเต็ม/ทศนิยมเอง (score >= 0 เสมอ)
    int whole = (int)score;
    int frac  = (int)((score - (float)whole) * 10000.0f + 0.5f);
    if (frac > 9999) { whole += 1; frac = 0; }
    char line[64];
    int n = snprintf(line, sizeof(line), "%u,%d.%04d,%d,%d\n",
                     (unsigned)(esp_timer_get_time() / 1000),
                     whole, frac, present ? 1 : 0, calibrating ? 1 : 0);
    if (n <= 0) return;
    if (log_len_ + n > CSI_LOG_BUF) {              // buffer เต็ม → flush เป็นก้อน
        fwrite(log_buf_, 1, log_len_, log_fp_);
        fflush(log_fp_);
        log_len_ = 0;
    }
    memcpy(log_buf_ + log_len_, line, n);
    log_len_ += n;
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
    LoadSenderMac();
    LoadThresholds();       // ถ้าเคยเรียนไว้แล้ว ใช้เลย ไม่ต้องรอ 15 วิ
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

    StartLogging();     // ถ้าไม่มี SD จะข้ามเอง ไม่พัง

    ESP_LOGI(TAG, "started — calibrating %d frames (~%d วิ)",
             CSI_CALIB_FRAMES, CSI_CALIB_FRAMES / CSI_PING_HZ);
    return true;
}

void CsiSensor::Stop() {
    if (!running_) return;
    running_ = false;
    StopLogging();
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

    // ล้างสถิติ Welford + ambient ด้วย
    memset(wf_mean_, 0, sizeof(wf_mean_));
    memset(wf_m2_,   0, sizeof(wf_m2_));
    wf_n_ = 0;
    top_k_count_ = 0;
    amb_sum_ = amb_sq_ = 0; amb_n_ = 0;
    ambient_done_ = false;
    thr_high_ = thr_low_ = 0.0f;
    last_frame_us_ = 0;
    csi_rate_hz_ = 0.0f;

    bp_ready_ = false;
    breath_len_ = breath_idx_ = 0;
    decim_acc_ = 0.0f; decim_n_ = 0;
    breath_bpm_ = 0.0f; bpm_tick_ = 0;
}

// ---------------- โหมด sender ----------------
bool CsiSensor::UsingSender() const {
    if (!sender_set_) return false;
    return (esp_timer_get_time() - last_sender_us_) < 3000000;
}

void CsiSensor::LoadSenderMac() {
    Settings st(NVS_NS, false);
    std::string m = st.GetString("sender", "");
    sender_set_ = false;
    if (m.size() >= 17) {
        unsigned v[6];
        if (sscanf(m.c_str(), "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) == 6) {
            for (int i = 0; i < 6; i++) sender_mac_[i] = (uint8_t)v[i];
            sender_set_ = true;
            ESP_LOGI(TAG, "ใช้ sender MAC ที่ตั้งไว้");
        }
    }
}

bool CsiSensor::SetSenderMac(const char* mac_str) {
    unsigned v[6];
    if (!mac_str || sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) {
        ESP_LOGE(TAG, "MAC ไม่ถูกรูปแบบ (ต้องเป็น aa:bb:cc:dd:ee:ff)");
        return false;
    }
    for (int i = 0; i < 6; i++) sender_mac_[i] = (uint8_t)v[i];
    sender_set_ = true;
    Settings st(NVS_NS, true);
    st.SetString("sender", mac_str);
    ForgetThresholds();       // แหล่งสัญญาณเปลี่ยน → ค่า ambient เดิมใช้ไม่ได้แล้ว
    ESP_LOGI(TAG, "ตั้ง sender แล้ว — ต้องเรียน ambient ใหม่");
    return true;
}

void CsiSensor::ClearSenderMac() {
    sender_set_ = false;
    Settings st(NVS_NS, true);
    st.SetString("sender", "");
    ForgetThresholds();
    ESP_LOGI(TAG, "กลับไปใช้ router mode");
}

// บังคับเรียน ambient ใหม่ทั้งหมด (ลบค่าใน NVS ด้วย)
void CsiSensor::ForgetThresholds() {
    Settings st(NVS_NS, true);
    st.SetInt("thr_high", 0);
    ambient_done_ = false;
    amb_sum_ = amb_sq_ = 0; amb_n_ = 0;
    thr_high_ = thr_low_ = 0.0f;
    ESP_LOGI(TAG, "ล้าง threshold แล้ว — จะเรียน ambient ใหม่");
}
