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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_wifi.h>

#define CSI_MAX_SUBCARRIERS   64      // LLTF = 64 subcarrier (buf 128 bytes)
#define CSI_WINDOW            8       // sliding window (ตรงกับ Python window=8)
#define CSI_CALIB_FRAMES      200     // จำนวน frame ห้องว่างสำหรับ baseline
#define CSI_PING_HZ           50      // ping router 50 ครั้ง/วิ → CSI ~50 Hz
#define CSI_QUEUE_LEN         8

class CsiSensor {
public:
    // score = motion score, present = มีคน, calibrating = กำลังเก็บ baseline
    using Callback = std::function<void(float score, bool present, bool calibrating)>;

    static CsiSensor& GetInstance();

    // เริ่ม/หยุดการตรวจจับ (เรียกตอนเปิด/ปิด radar)
    bool Start(Callback cb);
    void Stop();
    bool IsRunning() const { return running_; }

    // เก็บ baseline ใหม่ (เรียกเมื่อย้ายห้อง/เปลี่ยนสภาพแวดล้อม)
    void Recalibrate();

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

    // --- hysteresis (ตรงกับ Python detect_presence) ---
    bool  present_ = false;
    int   streak_  = 0;
};

#endif // CSI_SENSOR_H
