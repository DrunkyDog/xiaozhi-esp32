#include "avatar_player.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <src/misc/cache/instance/lv_image_cache.h>  // lv_image_cache_drop(), not exported by lvgl.h

#include <atomic>
#include <cstring>

#define TAG "AvatarPlayer"

namespace {

constexpr const char* kPartitionLabel = "avatar";
constexpr uint32_t kTickMs = 33;  // ~30 fps; only the face patch is redrawn
constexpr int kBlinkSeq[] = {1, 2, 2, 1};
constexpr int kBlinkMinMs = 2500;
constexpr int kBlinkRandMs = 3000;
constexpr int kTalkLevel = 6;         // speaker level (0..100) that counts as speech
constexpr int kVisemeMs = 90;         // how long one mouth shape is held while talking
constexpr int kMouthReleaseMs = 120;  // keep talking this long after the level drops
constexpr int kLevelStaleMs = 150;    // no audio written for this long = silence

// Mouth states in the pack: 0 mood, 1 a, 2 i, 3 u, 4 e, 5 o
constexpr int kLoudVisemes[] = {1, 5, 1, 4, 1, 5};  // open vowels dominate loud speech
constexpr int kSoftVisemes[] = {2, 4, 3, 2, 3, 4};  // narrow vowels for quiet speech

std::atomic<int> s_level{0};
std::atomic<int64_t> s_level_ms{0};

int64_t NowMs() { return esp_timer_get_time() / 1000; }

#pragma pack(push, 1)
struct PackHeader {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint16_t mood_count;
    uint16_t eye_states;
    uint16_t mouth_states;
    uint16_t entry_size;
};
#pragma pack(pop)

lv_image_dsc_t MakeDsc(const uint8_t* data, int w, int h) {
    lv_image_dsc_t dsc = {};
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc.header.w = w;
    dsc.header.h = h;
    dsc.header.stride = w * 2;  // colour plane; the alpha plane follows it
    dsc.data_size = w * h * 3;
    dsc.data = data;
    return dsc;
}

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

}  // namespace

void AvatarPlayer::SetOutputLevel(int level) {
    s_level.store(level, std::memory_order_relaxed);
    s_level_ms.store(NowMs(), std::memory_order_relaxed);
}

bool AvatarPlayer::Load() {
    if (partition_ != nullptr) {
        return true;
    }
    if (load_failed_) {
        return false;  // do not re-read a missing or bad pack on every mood change
    }
    load_failed_ = true;

    auto part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
    if (part == nullptr) {
        return false;  // layout without an avatar partition
    }
    PackHeader hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK || memcmp(hdr.magic, "AVT2", 4) != 0) {
        ESP_LOGW(TAG, "No avatar pack in partition '%s'", kPartitionLabel);
        return false;
    }
    const size_t face_count = size_t(hdr.eye_states) * hdr.mouth_states;
    const size_t entry_size = 16 + 4 + 4 * 2 + 4 * face_count;
    if (hdr.entry_size != entry_size || hdr.mood_count == 0 || face_count == 0) {
        ESP_LOGE(TAG, "Bad avatar header");
        return false;
    }

    std::vector<uint8_t> table(entry_size * hdr.mood_count);
    if (esp_partition_read(part, sizeof(hdr), table.data(), table.size()) != ESP_OK) {
        return false;
    }
    const size_t body_bytes = size_t(hdr.width) * hdr.height * 3;
    std::vector<Entry> entries(hdr.mood_count);
    for (int i = 0; i < hdr.mood_count; i++) {
        const uint8_t* e = table.data() + i * entry_size;
        Entry& m = entries[i];
        memcpy(m.name, e, sizeof(m.name));
        m.name[sizeof(m.name) - 1] = '\0';
        m.body = ReadU32(e + 16);
        m.face_x = ReadU16(e + 20);
        m.face_y = ReadU16(e + 22);
        m.face_w = ReadU16(e + 24);
        m.face_h = ReadU16(e + 26);
        m.faces.resize(face_count);
        uint32_t end = m.body + body_bytes;
        const size_t face_bytes = size_t(m.face_w) * m.face_h * 3;
        for (size_t f = 0; f < face_count; f++) {
            m.faces[f] = ReadU32(e + 28 + 4 * f);
            if (m.faces[f] < m.body || m.faces[f] + face_bytes > part->size) {
                ESP_LOGE(TAG, "Face %u of %s out of range", unsigned(f), m.name);
                return false;
            }
            if (m.faces[f] + face_bytes > end) {
                end = m.faces[f] + face_bytes;
            }
        }
        if (m.face_x + m.face_w > hdr.width || m.face_y + m.face_h > hdr.height || end > part->size) {
            ESP_LOGE(TAG, "Mood %s out of range", m.name);
            return false;
        }
        m.start = m.body;
        m.length = end - m.body;
    }

    entries_ = std::move(entries);
    width_ = hdr.width;
    height_ = hdr.height;
    eye_states_ = hdr.eye_states;
    mouth_states_ = hdr.mouth_states;
    partition_ = part;
    load_failed_ = false;
    ESP_LOGI(TAG, "Avatar pack: %d moods, %dx%d", hdr.mood_count, hdr.width, hdr.height);
    return true;
}

bool AvatarPlayer::Show(lv_obj_t* image, const char* mood) {
    if (image == nullptr || mood == nullptr || !Load()) {
        return false;
    }
    const Entry* found = nullptr;
    for (const auto& m : entries_) {
        if (strcmp(m.name, mood) == 0) {
            found = &m;
            break;
        }
    }
    if (found == nullptr) {
        return false;
    }
    if (found == current_ && lv_image_get_src(image) == &body_dsc_[active_]) {
        return true;  // already showing this mood
    }

    // Read the mood into the buffer that is not on screen
    const int next = current_ == nullptr ? active_ : 1 - active_;
    if (buffer_size_[next] < found->length) {
        heap_caps_free(buffers_[next]);
        buffers_[next] = static_cast<uint8_t*>(heap_caps_malloc(found->length, MALLOC_CAP_SPIRAM));
        buffer_size_[next] = buffers_[next] != nullptr ? found->length : 0;
    }
    auto part = static_cast<const esp_partition_t*>(partition_);
    if (buffers_[next] == nullptr ||
        esp_partition_read(part, found->start, buffers_[next], found->length) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot load mood %s (%u bytes)", mood, unsigned(found->length));
        return false;
    }

    // Fresh descriptors for this buffer; drop any cache entries left from its previous mood
    const uint8_t* base = buffers_[next] - found->start;
    lv_image_cache_drop(&body_dsc_[next]);
    for (auto& dsc : face_dscs_[next]) {
        lv_image_cache_drop(&dsc);
    }
    body_dsc_[next] = MakeDsc(base + found->body, width_, height_);
    face_dscs_[next].clear();
    for (uint32_t off : found->faces) {
        face_dscs_[next].push_back(MakeDsc(base + off, found->face_w, found->face_h));
    }

    active_ = next;
    current_ = found;
    lv_image_set_src(image, &body_dsc_[active_]);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);

    if (face_ == nullptr || lv_obj_get_parent(face_) != image) {
        face_ = lv_image_create(image);  // child of the body: positioned in body pixels
    }
    lv_obj_set_pos(face_, found->face_x, found->face_y);
    lv_obj_remove_flag(face_, LV_OBJ_FLAG_HIDDEN);
    eye_ = mouth_ = -1;  // force the first SetFace() to point at the new buffer
    SetFace(0, 0);

    if (timer_ == nullptr) {
        next_blink_ms_ = NowMs() + kBlinkMinMs;
        timer_ = lv_timer_create(TimerCb, kTickMs, this);
    }
    return true;
}

void AvatarPlayer::Hide() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (face_ != nullptr) {
        lv_obj_add_flag(face_, LV_OBJ_FLAG_HIDDEN);
    }
    current_ = nullptr;
    blink_step_ = -1;
}

void AvatarPlayer::TimerCb(lv_timer_t* timer) {
    static_cast<AvatarPlayer*>(lv_timer_get_user_data(timer))->Tick();
}

void AvatarPlayer::SetFace(int eye, int mouth) {
    if (current_ == nullptr || face_ == nullptr || (eye == eye_ && mouth == mouth_)) {
        return;  // unchanged: nothing is invalidated
    }
    eye_ = eye;
    mouth_ = mouth;
    lv_image_set_src(face_, &face_dscs_[active_][eye * mouth_states_ + mouth]);
}

void AvatarPlayer::Tick() {
    if (current_ == nullptr) {
        return;
    }
    const int64_t now = NowMs();

    // Blink: open -> half -> closed -> closed -> half -> open, every 2.5-5.5 s
    int eye = 0;
    if (blink_step_ < 0 && now >= next_blink_ms_) {
        blink_step_ = 0;
    }
    if (blink_step_ >= 0) {
        eye = kBlinkSeq[blink_step_++];
        if (blink_step_ >= int(sizeof(kBlinkSeq) / sizeof(kBlinkSeq[0]))) {
            blink_step_ = -1;
            next_blink_ms_ = now + kBlinkMinMs + int(esp_random() % kBlinkRandMs);
        }
    }

    // Lip-sync from the speaker level: a new viseme every ~90 ms while it is above the threshold
    int level = s_level.load(std::memory_order_relaxed);
    if (now - s_level_ms.load(std::memory_order_relaxed) > kLevelStaleMs) {
        level = 0;
    }
    int mouth = 0;
    if (level >= kTalkLevel) {
        mouth_hold_until_ms_ = now + kMouthReleaseMs;
    }
    if (now < mouth_hold_until_ms_) {
        if (now >= next_viseme_ms_) {
            next_viseme_ms_ = now + kVisemeMs;
            viseme_ = (viseme_ + 1 + int(esp_random() % 2)) % 6;
        }
        // Short dips between syllables fall back to narrow vowels instead of snapping shut
        mouth = level >= 3 * kTalkLevel ? kLoudVisemes[viseme_] : kSoftVisemes[viseme_];
    }
    if (mouth >= mouth_states_ || eye >= eye_states_) {
        return;
    }
    SetFace(eye, mouth);
}
