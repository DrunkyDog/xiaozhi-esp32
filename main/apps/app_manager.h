#ifndef APP_MANAGER_H
#define APP_MANAGER_H

// AppManager — ตัวกลางเปิด/ปิดแอปเต็มจอ (RadarApp, FlightRadarApp, ...)
// กติกา: เปิดได้ทีละแอปเดียว — เปิดแอปใหม่จะปิดแอปเดิมก่อนเสมอ
// เพื่อให้แอปใหม่จำ "จอผู้ช่วย" เป็นจอที่ต้องกลับ ไม่ใช่จอของแอปก่อนหน้า

#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <lvgl.h>

class AppManager {
public:
    struct App {
        std::string name;                  // ชื่อที่ใช้สั่ง เช่น "radar"
        std::function<void()> show;
        std::function<void()> hide;
        std::function<bool()> is_visible;  // ใช้ตรวจว่า Show() สำเร็จจริง
    };

    static AppManager& GetInstance();

    void Register(App app);

    // เปิดแอปตามชื่อ (ปิดแอปที่เปิดอยู่ก่อน) — false ถ้าไม่รู้จักชื่อหรือเปิดไม่สำเร็จ
    bool Open(const std::string& name);
    // ปิดเฉพาะเมื่อแอปนั้นเป็นแอปที่เปิดอยู่ — false ถ้าไม่ได้เปิดอยู่
    bool Close(const std::string& name);
    // ปิดแอปที่เปิดอยู่ (ถ้ามี) — false ถ้าไม่มีแอปเปิดอยู่
    bool CloseCurrent();

    // จอที่แอปควรกลับไปตอน Hide() — ถ้ามีจอกำลัง fade-in อยู่ (เช่นเพิ่งปิดแอปก่อนหน้า)
    // ให้ใช้จอนั้น เพราะ lv_screen_active() ยังเป็นจอเก่าจนกว่า animation จะเริ่ม
    // ต้องเรียกขณะถือ DisplayLockGuard
    static lv_obj_t* ReturnScreen() {
        lv_obj_t* loading = lv_display_get_screen_loading(nullptr);
        return loading ? loading : lv_screen_active();
    }

    std::string Current();                 // "" = อยู่ที่จอผู้ช่วย
    std::vector<std::string> Names();

private:
    AppManager();
    int  Find(const std::string& name);   // -1 = ไม่พบ
    void HideCurrentLocked();

    std::mutex mutex_;
    std::vector<App> apps_;
    int current_ = -1;                     // index ใน apps_, -1 = ไม่มีแอปเปิด
};

#endif // APP_MANAGER_H
