#ifndef AVATAR_PLAYER_H
#define AVATAR_PLAYER_H

#include <lvgl.h>

#include <cstdint>
#include <vector>

// Layered mood avatar stored in the "avatar" data partition (format AVT2, produced by
// alice-watch-os/tools/avatar/pack.py). Each mood has a full body frame plus small face patches
// for 3 eye states x 6 mouth states, raw RGB565A8.
//
// The partition sits above the 16MB that ESP32-S3 quad flash can mmap, so a mood's data
// (~320KB, stored contiguously) is read into PSRAM when the mood changes. Animation then only
// swaps the face patch, redrawing a ~56x48 area per frame.
//
// All methods except SetOutputLevel() must run with the display lock held (LVGL context).
class AvatarPlayer {
public:
    // Reads the pack index. Safe to call repeatedly; returns true once loaded.
    bool Load();

    // Shows `mood` in `image` (body) with the face overlay on top. Returns false when there is
    // no pack or it has no such mood, leaving the caller to fall back to the emoji collection.
    bool Show(lv_obj_t* image, const char* mood);
    void Hide();

    // Speaker level 0..100 from the audio output path (any task). Drives lip-sync.
    static void SetOutputLevel(int level);

private:
    struct Entry {
        char name[16];
        uint32_t start;     // first byte of this mood's blobs in the partition
        uint32_t length;    // body + all face patches (contiguous)
        uint32_t body;      // offsets relative to the partition
        uint16_t face_x, face_y, face_w, face_h;
        std::vector<uint32_t> faces;
    };

    static void TimerCb(lv_timer_t* timer);
    void Tick();
    void SetFace(int eye, int mouth);

    const void* partition_ = nullptr;  // esp_partition_t*
    bool load_failed_ = false;
    uint16_t width_ = 0, height_ = 0;
    int eye_states_ = 0;
    int mouth_states_ = 0;
    std::vector<Entry> entries_;

    // Double buffer: the image being shown keeps pointing into its buffer until the next
    // mood's data is fully read into the other one.
    uint8_t* buffers_[2] = {nullptr, nullptr};
    size_t buffer_size_[2] = {0, 0};
    int active_ = 0;

    // One descriptor set per buffer, so a mood switch always hands LVGL new source pointers
    // (its image cache is keyed by the source pointer).
    const Entry* current_ = nullptr;
    lv_image_dsc_t body_dsc_[2] = {};
    std::vector<lv_image_dsc_t> face_dscs_[2];
    lv_obj_t* face_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    int eye_ = -1;
    int mouth_ = -1;
    int blink_step_ = -1;
    int64_t next_blink_ms_ = 0;
    int64_t next_viseme_ms_ = 0;
    int64_t mouth_hold_until_ms_ = 0;
    int viseme_ = 0;
};

#endif  // AVATAR_PLAYER_H
