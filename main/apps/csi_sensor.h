/*
 * CsiSensor — WiFi CSI presence/motion sensing บน ESP32-S3 (on-device)
 *
 * แทนที่ pipeline ฝั่ง Python ทั้งหมด — ไม่ต้องมี backend/Mac อีกต่อไป
 * อิงวิธีจาก espressif/esp-csi → examples/get-started/csi_recv_router
 *
 * หลักการ:
 *   1. ใช้การเชื่อมต่อ WiFi ที่ Alice ต่ออยู่แล้ว (STA) — ไม่ต้องมีบอร์ดเพิ่ม
 *   2. esp_wifi_set_csi_rx_cb() ดัก CSI ของ packet ที่มาจาก router (กรองด้วย BSSID)
 *   3. ping gateway ถี่ ๆ เพื่อให้มี packet ไหลสม่ำเสมอ → CSI rate นิ่ง
 *   4. callback (WiFi task) copy raw buffer ลง queue เท่านั้น — ห้ามช้า
 *   5. task ของเราคำนวณ amplitude → baseline → sliding variance → motion score
 *
 * การประมวลผลตรงกับฝั่ง Python (backend/processing.py) ทุกขั้น
 */
#ifndef CSI_SENSOR_H
#define CSI_SENSOR_H

#include <functional>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_wifi.h>

#define CSI_MAX_SUBCARRIERS   64      // LLTF = 64 subcarrier (buf 128 bytes)
#define CSI_WINDOW            8       // sliding window (ตรงกับ Python window=8)
#define CSI_CALIB_FRAMES      200     // stage 1: baseline ต่อ subcarrier (~4 วิ)

// --- adaptive threshold (แนวทางจาก ruvnet/RuView, MIT/Apache-2.0) ---
// เดิมใช้ค่าคงที่ 3.0 ที่จูนจาก simulator ซึ่งไม่ตรงกับห้องจริง
// ตอนนี้เรียนรู้ ambient เอง: threshold = mean + 3*sigma ของ score ตอนห้องว่าง
#define CSI_AMBIENT_FRAMES    750     // stage 2: เรียน ambient (~15 วิ ที่ 50Hz)
#define CSI_SIGMA_MULT        3.0f    // threshold_high = mean + 3*sigma
#define CSI_HYST_RATIO        0.5f    // threshold_low = 0.5 * high (ออกยากกว่าเข้า)
#define CSI_CLEAR_FRAMES      5       // ต่ำกว่า low กี่ frame ติดกันถึงประกาศว่าง
#define CSI_TOP_K             8       // ใช้เฉพาะ subcarrier ที่แกว่งมากสุด K ตัว

// --- ตรวจอัตราการหายใจ (แนวทางจาก RuView: bandpass 0.1-0.5Hz + zero-crossing) ---
// การหายใจ 6-30 BPM = 0.1-0.5 Hz → รอบละ 2-10 วินาที
// ต้องมีประวัติยาวหลายรอบถึงจะวัดได้ → decimate ลงเหลือ 10 Hz
// 256 sample @10Hz = 25 วินาที (พอ) เทียบกับ 256 @50Hz = 5 วินาที (ไม่พอสักรอบ)
#define CSI_BREATH_FS         10.0f   // อัตราหลัง decimate
#define CSI_BREATH_HIST       256     // = 25 วินาที
#define CSI_BREATH_LO_HZ      0.1f    // 6 BPM
#define CSI_BREATH_HI_HZ      0.5f    // 30 BPM
#define CSI_PING_HZ           50      // ping router 50 ครั้ง/วิ → CSI ~50 Hz
#define CSI_QUEUE_LEN         8
#define CSI_LOG_BUF           2048    // buffer ก่อนเขียน SD (flush เป็นก้อน)

class CsiSensor {
public:
    // score = motion score, present = มีคน, calibrating = กำลังเก็บ baseline
    using Callback = std::function<void(float score, bool present, bool calibrating)>;

    static CsiSensor& GetInstance();

    // เริ่ม/หยุดการตรวจจับ (เรียกตอนเปิด/ปิด radar)
    bool Start(Callback cb);
    void Stop();
    bool IsRunning() const { return running_; }

    // เก็บ baseline + เรียน ambient ใหม่ (เรียกเมื่อย้ายห้อง/เปลี่ยนสภาพแวดล้อม)
    // ล้างค่าที่เก็บใน NVS ด้วย เพื่อบังคับให้เรียนใหม่ทั้งหมด
    void Recalibrate();
    void ForgetThresholds();               // ลบค่าใน NVS → บังคับเรียนใหม่

    // ตั้ง MAC ของบอร์ดที่ทำหน้าที่ยิง packet (เช่น ESP32-C3 ที่ flash csi_send)
    // ถ้าตั้งไว้ จะใช้ CSI จากตัวนั้นแทน router — สัญญาณนิ่งกว่ามาก
    // ถ้าไม่ได้รับจากมันเกิน 3 วิ (เช่นไฟดับ) จะกลับไปใช้ router เองอัตโนมัติ
    bool SetSenderMac(const char* mac_str);   // "aa:bb:cc:dd:ee:ff"
    void ClearSenderMac();
    bool HasSender() const { return sender_set_; }
    bool UsingSender() const;
    float ThresholdHigh() const { return thr_high_; }
    // อัตราการหายใจ (BPM) — 0 = ยังวัดไม่ได้/ไม่มีคนนิ่งพอ
    float BreathingBpm() const { return breath_bpm_; }
    int   Subcarriers() const { return n_sub_; }
    bool  IsCalibrated() const { return calibrated_ && ambient_done_; }
    float CsiRateHz() const { return csi_rate_hz_; }

    // บันทึก score ลง SD เพื่อเอาไปวิเคราะห์/จูน threshold บน Mac
    // ไฟล์: /sdcard/CSInnnnn.CSV (ชื่อ 8.3 เพราะ CONFIG_FATFS_LFN_NONE)
    // คอลัมน์: ms,score,present,calibrating
    bool StartLogging();
    void StopLogging();
    bool IsLogging() const { return log_fp_ != nullptr; }
    const char* LogPath() const { return log_path_; }

private:
    CsiSensor() = default;

    struct RawFrame {
        int8_t   buf[CSI_MAX_SUBCARRIERS * 2];
        uint16_t len;
        float    gain;
    };

    static void CsiRxCb(void* ctx, wifi_csi_info_t* info);   // WiFi task — เร็วที่สุด
    static void ProcTask(void* arg);
    void Process(const RawFrame& f);
    float MotionScore();
    bool  DetectPresence(float score);

    Callback       cb_;
    volatile bool  running_ = false;
    TaskHandle_t   task_    = nullptr;
    QueueHandle_t  queue_   = nullptr;
    uint8_t        bssid_[6] = {0};

    // --- โหมด sender เฉพาะ (ESP32-C3 ยิง packet ให้) ---
    void LoadSenderMac();
    uint8_t          sender_mac_[6] = {0};
    bool             sender_set_    = false;
    volatile int64_t last_sender_us_ = 0;

    // --- processing state ---
    int   n_sub_ = 0;
    float window_[CSI_WINDOW][CSI_MAX_SUBCARRIERS] = {};
    int   win_count_ = 0;                 // จำนวน frame ใน window (สูงสุด CSI_WINDOW)
    int   win_head_  = 0;

    float base_mean_[CSI_MAX_SUBCARRIERS] = {};
    float base_std_[CSI_MAX_SUBCARRIERS]  = {};
    float calib_sum_[CSI_MAX_SUBCARRIERS] = {};
    float calib_sq_[CSI_MAX_SUBCARRIERS]  = {};
    int   calib_n_    = 0;
    bool  calibrated_ = false;

    // --- adaptive threshold ---
    void SelectTopK();
    void LoadThresholds();                 // อ่านจาก NVS
    void SaveThresholds();                 // เก็บลง NVS หลังเรียนเสร็จ

    // Welford online variance ต่อ subcarrier — ไม่ต้องเก็บ buffer ยาว
    float wf_mean_[CSI_MAX_SUBCARRIERS] = {};
    float wf_m2_[CSI_MAX_SUBCARRIERS]   = {};
    uint32_t wf_n_ = 0;
    uint8_t  top_k_[CSI_TOP_K] = {};
    uint8_t  top_k_count_ = 0;

    // เรียน ambient (stage 2)
    bool   ambient_done_ = false;
    double amb_sum_ = 0, amb_sq_ = 0;
    uint32_t amb_n_ = 0;
    float  thr_high_ = 0.0f;
    float  thr_low_  = 0.0f;

    // วัดอัตรา CSI จริง — ping 50Hz ไม่ได้แปลว่า callback มา 50Hz
    // ค่านี้จำเป็นสำหรับคำนวณ BPM ให้ถูกในเฟสถัดไป
    int64_t last_frame_us_ = 0;
    float   csi_rate_hz_   = 0.0f;

    // --- breathing ---
    struct Biquad { float b0,b1,b2,a1,a2, x1,x2,y1,y2; };
    void  BiquadDesign(Biquad& bq, float fs, float f_lo, float f_hi);
    float BiquadApply(Biquad& bq, float x);
    void  FeedBreathing(float sample);
    float EstimateBpm();

    Biquad bp_ = {};
    bool   bp_ready_ = false;
    float  breath_hist_[CSI_BREATH_HIST] = {};
    int    breath_len_ = 0, breath_idx_ = 0;
    float  decim_acc_ = 0.0f;
    int    decim_n_ = 0, decim_target_ = 5;   // ปรับตามอัตราจริง
    float  breath_bpm_ = 0.0f;
    int    bpm_tick_ = 0;

    // --- hysteresis 2 ระดับ ---
    bool  present_ = false;
    int   streak_  = 0;

    // --- logging ลง SD ---
    void LogSample(float score, bool present, bool calibrating);
    FILE* log_fp_ = nullptr;
    char  log_path_[32] = {0};
    char  log_buf_[CSI_LOG_BUF];      // buffer ก่อน flush — เขียนทุก frame จะช้ามาก
    int   log_len_ = 0;
};

#endif // CSI_SENSOR_H
