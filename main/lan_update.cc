#include "lan_update.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <cstring>
#include <memory>

#include "application.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#define TAG "LanUpdate"

static constexpr int64_t kWindowUs = 5LL * 60 * 1000 * 1000;  // 5 minutes
static constexpr int kMaxBadPins = 3;
static constexpr size_t kChunk = 4096;
static constexpr const char* kNvsNs = "lan_update";

// Same pairing rule as Ota::Upgrade(): next to the Launcher (ota_0/ota_1) this
// firmware owns ota_2/ota_3 and must stay inside its own A/B pair.
static const esp_partition_t* PartnerSlot() {
    auto running = esp_ota_get_running_partition();
    if (running != nullptr && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_2 &&
        running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        int slot = running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
        auto partner = (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + (slot ^ 1));
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, partner, nullptr);
    }
    return esp_ota_get_next_update_partition(nullptr);
}

static const esp_partition_t* AssetsPartition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
}

static esp_err_t Reply(httpd_req_t* req, const char* status, const char* text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}

// Receive exactly `len` bytes (or fewer at the end of the body). Returns bytes read, -1 on error.
static int RecvChunk(httpd_req_t* req, char* buf, size_t len) {
    for (int retries = 0; retries < 5; retries++) {
        int r = httpd_req_recv(req, buf, len);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        return r < 0 ? -1 : r;
    }
    return -1;
}

LanUpdate& LanUpdate::GetInstance() {
    static LanUpdate instance;
    return instance;
}

void LanUpdate::ShowMessage(const std::string& text) {
    ESP_LOGI(TAG, "%s", text.c_str());
    Application::GetInstance().Schedule([text]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display) display->SetChatMessage("system", text.c_str());
    });
}

void LanUpdate::RebootSoon() {
    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }, "lan_reboot", 2048, nullptr, 5, nullptr);
}

void LanUpdate::Toggle() {
    if (IsActive()) {
        Stop();
        ShowMessage("ปิดโหมดอัปเดตผ่าน LAN แล้ว");
    } else {
        Start();
    }
}

bool LanUpdate::Start() {
    if (IsActive()) return true;
    std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (ip.empty() || ip == "0.0.0.0") {
        ShowMessage("อัปเดตผ่าน LAN: ยังไม่ได้ต่อ Wi-Fi");
        return false;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32770;
    config.max_open_sockets = 2;
    config.max_uri_handlers = 3;
    config.stack_size = 6144;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;
    config.lru_purge_enable = true;
    if (httpd_start(&server_, &config) != ESP_OK) {
        server_ = nullptr;
        ShowMessage("อัปเดตผ่าน LAN: เปิด web server ไม่ได้");
        return false;
    }
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = this};
    httpd_uri_t app = {.uri = "/app", .method = HTTP_POST, .handler = HandleApp, .user_ctx = this};
    httpd_uri_t assets = {.uri = "/assets", .method = HTTP_POST, .handler = HandleAssets, .user_ctx = this};
    httpd_register_uri_handler(server_, &index);
    httpd_register_uri_handler(server_, &app);
    httpd_register_uri_handler(server_, &assets);

    snprintf(pin_, sizeof(pin_), "%06lu", (unsigned long)(esp_random() % 1000000));
    bad_pins_ = 0;

    if (timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = OnTimeout;
        args.arg = this;
        args.name = "lan_update";
        esp_timer_create(&args, &timer_);
    }
    esp_timer_stop(timer_);
    esp_timer_start_once(timer_, kWindowUs);

    ShowMessage("อัปเดตผ่าน LAN (5 นาที)\nhttp://" + ip + "/\nPIN " + pin_);
    return true;
}

void LanUpdate::Stop() {
    if (timer_) esp_timer_stop(timer_);
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    memset(pin_, 0, sizeof(pin_));
    ESP_LOGI(TAG, "stopped");
}

void LanUpdate::OnTimeout(void* arg) {
    // httpd_stop() joins the server task: run it on the main loop, not in the timer task
    Application::GetInstance().Schedule([]() {
        auto& self = LanUpdate::GetInstance();
        if (self.IsActive()) {
            self.Stop();
            self.ShowMessage("หมดเวลาโหมดอัปเดตผ่าน LAN");
        }
    });
}

bool LanUpdate::CheckPin(httpd_req_t* req) {
    char pin[8] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Pin", pin, sizeof(pin)) == ESP_OK && pin_[0] != '\0' &&
        strcmp(pin, pin_) == 0) {
        return true;
    }
    if (++bad_pins_ >= kMaxBadPins) {
        ESP_LOGW(TAG, "too many wrong PINs, closing");
        Application::GetInstance().Schedule([]() {
            LanUpdate::GetInstance().Stop();
            LanUpdate::GetInstance().ShowMessage("PIN ผิดเกินกำหนด ปิดโหมดอัปเดตแล้ว");
        });
    }
    return false;
}

esp_err_t LanUpdate::HandleIndex(httpd_req_t* req) {
    static const char kPage[] = R"HTML(<!doctype html><html lang="th"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>AI Voice update</title>
<style>body{font-family:system-ui,sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem}
fieldset{margin:1rem 0;border-radius:8px}button{padding:.5rem 1rem}#log{white-space:pre-wrap}</style>
<h1>AI Voice · อัปเดตผ่าน LAN</h1>
<p>PIN (แสดงบนจอเครื่อง) <input id="pin" inputmode="numeric" maxlength="6" size="8"></p>
<fieldset><legend>1. Assets (ถ้ามี) — <code>generated_assets.bin</code></legend>
<input type="file" id="assets"> <button onclick="up('assets')">อัปโหลด assets</button>
<p><small>เครื่องจะรีบูตเพื่อติดตั้ง assets แล้วเปิดหน้านี้ให้อีกรอบอัตโนมัติ</small></p></fieldset>
<fieldset><legend>2. Firmware — <code>xiaozhi.bin</code></legend>
<input type="file" id="app"> <button onclick="up('app')">อัปโหลด firmware</button></fieldset>
<p id="log"></p>
<script>
async function up(kind){const f=document.getElementById(kind).files[0],log=document.getElementById('log');
if(!f){log.textContent='เลือกไฟล์ก่อน';return}
log.textContent='กำลังส่ง '+f.name+' ('+f.size+' B)...';
try{const r=await fetch('/'+kind,{method:'POST',headers:{'X-Pin':document.getElementById('pin').value},body:f});
log.textContent=(r.ok?'สำเร็จ: ':'ผิดพลาด: ')+await r.text()}catch(e){log.textContent='ส่งไม่สำเร็จ: '+e}}
</script></html>)HTML";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
}

esp_err_t LanUpdate::HandleApp(httpd_req_t* req) {
    auto& self = *static_cast<LanUpdate*>(req->user_ctx);
    if (!self.CheckPin(req)) return Reply(req, "403 Forbidden", "PIN ไม่ถูกต้อง");

    auto partner = PartnerSlot();
    size_t total = req->content_len;
    if (partner == nullptr) return Reply(req, "500 Internal Server Error", "ไม่พบช่อง OTA");
    if (total == 0 || total > partner->size) return Reply(req, "413 Payload Too Large", "ขนาดไฟล์ไม่ถูกต้อง");

    std::unique_ptr<char[]> buf(new (std::nothrow) char[kChunk]);
    if (!buf) return Reply(req, "500 Internal Server Error", "หน่วยความจำไม่พอ");

    self.ShowMessage("กำลังรับ firmware...");
    esp_ota_handle_t handle = 0;
    bool begun = false;
    size_t received = 0;
    int last_pct = -1;
    while (received < total) {
        int r = RecvChunk(req, buf.get(), std::min(kChunk, total - received));
        if (r <= 0) break;
        if (!begun) {
            if ((uint8_t)buf[0] != 0xE9) {
                return Reply(req, "400 Bad Request", "ไม่ใช่ไฟล์ firmware ของ ESP32 (xiaozhi.bin)");
            }
            if (esp_ota_begin(partner, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
                return Reply(req, "500 Internal Server Error", "esp_ota_begin ล้มเหลว");
            }
            begun = true;
        }
        if (esp_ota_write(handle, buf.get(), r) != ESP_OK) {
            esp_ota_abort(handle);
            return Reply(req, "500 Internal Server Error", "เขียน flash ล้มเหลว");
        }
        received += r;
        int pct = received * 100 / total;
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            self.ShowMessage("กำลังรับ firmware " + std::to_string(pct) + "%");
        }
    }
    if (!begun || received != total) {
        if (begun) esp_ota_abort(handle);
        return Reply(req, "400 Bad Request", "รับไฟล์ไม่ครบ");
    }
    if (esp_ota_end(handle) != ESP_OK) {
        return Reply(req, "400 Bad Request", "ไฟล์ firmware เสียหรือไม่สมบูรณ์");
    }
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(partner, &desc) != ESP_OK ||
        strcmp(desc.project_name, "xiaozhi") != 0) {
        return Reply(req, "400 Bad Request", "ไม่ใช่ firmware ของ AI Voice (project_name ไม่ใช่ xiaozhi)");
    }
    if (esp_ota_set_boot_partition(partner) != ESP_OK) {
        return Reply(req, "500 Internal Server Error", "ตั้งช่องบูตไม่สำเร็จ");
    }
    ESP_LOGI(TAG, "app %s (%s) written to %s", desc.version, desc.date, partner->label);
    Reply(req, "200 OK", ("ติดตั้ง firmware " + std::string(desc.version) + " แล้ว กำลังรีบูต").c_str());
    self.ShowMessage("ติดตั้ง firmware แล้ว กำลังรีบูต");
    self.RebootSoon();
    return ESP_OK;
}

esp_err_t LanUpdate::HandleAssets(httpd_req_t* req) {
    auto& self = *static_cast<LanUpdate*>(req->user_ctx);
    if (!self.CheckPin(req)) return Reply(req, "403 Forbidden", "PIN ไม่ถูกต้อง");

    auto partner = PartnerSlot();
    auto assets = AssetsPartition();
    size_t total = req->content_len;
    if (partner == nullptr || assets == nullptr) return Reply(req, "500 Internal Server Error", "ไม่พบ partition");
    if (total <= 12 || total > assets->size || total > partner->size) {
        return Reply(req, "413 Payload Too Large", "ขนาดไฟล์ assets ไม่ถูกต้อง");
    }
    // The partner slot is also the rollback target while a new image waits for confirmation.
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        return Reply(req, "409 Conflict", "firmware ปัจจุบันยังไม่ถูกยืนยัน รอให้เช็ค OTA ผ่านก่อนแล้วลองใหม่");
    }

    std::unique_ptr<char[]> buf(new (std::nothrow) char[kChunk]);
    if (!buf) return Reply(req, "500 Internal Server Error", "หน่วยความจำไม่พอ");

    self.ShowMessage("กำลังรับ assets...");
    size_t erase_len = (total + 4095) & ~4095u;
    if (esp_partition_erase_range(partner, 0, erase_len) != ESP_OK) {
        return Reply(req, "500 Internal Server Error", "ลบพื้นที่พักไฟล์ไม่สำเร็จ");
    }
    uint32_t hdr_files = 0, hdr_sum = 0, hdr_len = 0, sum = 0;
    size_t received = 0;
    int last_pct = -1;
    while (received < total) {
        int r = RecvChunk(req, buf.get(), std::min(kChunk, total - received));
        if (r <= 0) break;
        if (received == 0) {
            if (r < 12) return Reply(req, "400 Bad Request", "ส่วนหัวไฟล์ assets สั้นเกินไป");
            memcpy(&hdr_files, buf.get(), 4);
            memcpy(&hdr_sum, buf.get() + 4, 4);
            memcpy(&hdr_len, buf.get() + 8, 4);
            if (hdr_files == 0 || hdr_files > 1000 || 12 + (size_t)hdr_len != total) {
                return Reply(req, "400 Bad Request", "ไม่ใช่ไฟล์ assets (generated_assets.bin)");
            }
        }
        for (int i = 0; i < r; i++) {
            if (received + i >= 12) sum += (uint8_t)buf[i];
        }
        if (esp_partition_write(partner, received, buf.get(), r) != ESP_OK) {
            return Reply(req, "500 Internal Server Error", "เขียนพื้นที่พักไฟล์ไม่สำเร็จ");
        }
        received += r;
        int pct = received * 100 / total;
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            self.ShowMessage("กำลังรับ assets " + std::to_string(pct) + "%");
        }
    }
    if (received != total) return Reply(req, "400 Bad Request", "รับไฟล์ไม่ครบ");
    if ((sum & 0xFFFF) != hdr_sum) return Reply(req, "400 Bad Request", "checksum ของ assets ไม่ตรง");

    {
        Settings settings(kNvsNs, true);
        settings.SetInt("staged_len", (int32_t)total);
        settings.SetBool("reopen", true);
    }
    ESP_LOGI(TAG, "assets staged in %s: %u files, %u bytes", partner->label, (unsigned)hdr_files,
             (unsigned)total);
    Reply(req, "200 OK", "รับ assets แล้ว เครื่องจะรีบูตไปติดตั้ง แล้วเปิดหน้านี้ให้อีกครั้ง (PIN ใหม่)");
    self.ShowMessage("รับ assets แล้ว กำลังรีบูตไปติดตั้ง");
    self.RebootSoon();
    return ESP_OK;
}

void LanUpdate::ApplyStagedAssetsIfAny() {
    int32_t staged_len;
    {
        Settings settings(kNvsNs, true);
        staged_len = settings.GetInt("staged_len", 0);
        if (staged_len <= 0) return;
        settings.EraseKey("staged_len");  // one attempt only: never loop on a bad copy
    }
    auto partner = PartnerSlot();
    auto assets = AssetsPartition();
    if (partner == nullptr || assets == nullptr || (size_t)staged_len > assets->size) {
        ESP_LOGE(TAG, "staged assets: partition missing or too large");
        return;
    }
    std::unique_ptr<char[]> buf(new (std::nothrow) char[kChunk]);
    if (!buf) return;

    // Verify the staged copy again before touching the live assets partition
    uint32_t hdr[3] = {};
    uint32_t sum = 0;
    if (esp_partition_read(partner, 0, hdr, sizeof(hdr)) != ESP_OK || 12 + (size_t)hdr[2] != (size_t)staged_len) {
        ESP_LOGE(TAG, "staged assets: bad header");
        return;
    }
    for (size_t off = 12; off < (size_t)staged_len; off += kChunk) {
        size_t n = std::min(kChunk, (size_t)staged_len - off);
        if (esp_partition_read(partner, off, buf.get(), n) != ESP_OK) return;
        for (size_t i = 0; i < n; i++) sum += (uint8_t)buf[i];
    }
    if ((sum & 0xFFFF) != hdr[1]) {
        ESP_LOGE(TAG, "staged assets: checksum mismatch, keeping current assets");
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display) display->SetChatMessage("system", "กำลังติดตั้ง assets...");
    ESP_LOGI(TAG, "copying %ld bytes of staged assets into %s", (long)staged_len, assets->label);
    size_t erase_len = ((size_t)staged_len + 4095) & ~4095u;
    if (esp_partition_erase_range(assets, 0, erase_len) != ESP_OK) {
        ESP_LOGE(TAG, "erase assets failed");
        return;
    }
    for (size_t off = 0; off < (size_t)staged_len; off += kChunk) {
        size_t n = std::min(kChunk, (size_t)staged_len - off);
        if (esp_partition_read(partner, off, buf.get(), n) != ESP_OK ||
            esp_partition_write(assets, off, buf.get(), n) != ESP_OK) {
            ESP_LOGE(TAG, "copy failed at 0x%x", (unsigned)off);
            return;
        }
    }
    ESP_LOGI(TAG, "assets installed, rebooting");
    esp_restart();
}

void LanUpdate::ReopenIfRequested() {
    Settings settings(kNvsNs, true);
    if (!settings.GetBool("reopen", false)) return;
    settings.EraseKey("reopen");
    GetInstance().Start();
}
