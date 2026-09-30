#ifndef LAN_UPDATE_H
#define LAN_UPDATE_H

#include <esp_http_server.h>
#include <esp_timer.h>
#include <string>

// Firmware / assets update over the local network, without the OTA server.
//
// BOOT long press opens http://<device-ip>/ for 5 minutes. Uploads need the
// 6-digit PIN shown on the screen; 3 wrong PINs close the window.
//
// - app:    written to the partner slot of the running A/B pair, validated
//           (ESP image + project name), then booted. Rollback stays active:
//           the new image is confirmed by the normal OTA check.
// - assets: fonts and wake word models are read straight from the assets
//           partition while the device runs, so it cannot be rewritten live.
//           The upload is staged in the partner app slot (after its checksum
//           is verified) and copied into the assets partition on the next boot,
//           before anything maps it. The window reopens after that boot so the
//           app can be uploaded next.
class LanUpdate {
public:
    static LanUpdate& GetInstance();

    void Toggle();
    bool Start();
    void Stop();
    bool IsActive() const { return server_ != nullptr; }

    // Early in activation, before Assets::Apply(): copy staged assets into the
    // assets partition and reboot. Returns normally when nothing is staged.
    static void ApplyStagedAssetsIfAny();
    // After activation: reopen the window if a staged assets copy asked for it.
    static void ReopenIfRequested();

private:
    LanUpdate() = default;

    static esp_err_t HandleIndex(httpd_req_t* req);
    static esp_err_t HandleApp(httpd_req_t* req);
    static esp_err_t HandleAssets(httpd_req_t* req);
    static void OnTimeout(void* arg);

    bool CheckPin(httpd_req_t* req);
    void ShowMessage(const std::string& text);
    void RebootSoon();

    httpd_handle_t server_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
    char pin_[7] = {};
    int bad_pins_ = 0;
};

#endif  // LAN_UPDATE_H
