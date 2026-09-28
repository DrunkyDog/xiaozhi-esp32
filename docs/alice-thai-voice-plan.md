# ALICE — แผนงาน Thai Voice Bot บน ESP32-S3-Touch-AMOLED-2.06

> **เอกสาร hand-off ข้าม session** — สร้าง 2026-09-21
> อ่านไฟล์นี้ก่อนเริ่มงานต่อ ไม่ต้องรื้อ research ซ้ำ
> ทุกข้อในหมวด **FACT** ตรวจจากไฟล์จริงบนเครื่อง / repo จริงแล้ว
> หมวด **ASSUMPTION** = ยังไม่ทดสอบ / **OPINION** = ความเห็นเชิงออกแบบ

---

## 0. TL;DR สำหรับ session ใหม่

1. บอร์ดนี้ **upstream รองรับเต็ม** อยู่แล้ว ไม่ต้อง fork BSP
2. ภาษาไทย **รองรับอยู่แล้ว** (locale th-TH + Noto Sans Thai + เสียง .ogg ไทย) — premise เดิมที่ว่า "ไม่รองรับไทย" **ผิด**
3. ช่องว่างจริงของไทยเหลือ **3 เรื่องเท่านั้น**: wake word (แก้ไม่ได้), mark positioning (ต้องทดสอบ), cloud ASR/TTS (แก้ด้วย self-host — ทำไปแล้วบางส่วน)
4. เครื่องนี้ **self-host อยู่แล้ว** ที่ `alice.project-alice.uk` — งานที่เหลือคือ "เสริม" ไม่ใช่ "ย้าย"
5. งานถัดไปที่คุ้มที่สุด = **SDL2 host simulator** (จาก andygeiss/esp32-watch) เพื่อ debug การวางวรรณยุกต์ไทยโดยไม่ต้อง flash

---

## 1. สภาพจริงของเครื่อง (FACT — ตรวจแล้ว 2026-09-21)

### 1.1 Repo หลัก
| รายการ | ค่า |
|---|---|
| path | `/Users/the1stladylawrence/xiaozhi-esp32` |
| remote | `https://github.com/78/xiaozhi-esp32.git` |
| HEAD | `fd80e84 feat: WiFi CSI radar app with voice-controlled screen takeover` |
| commit ของเราล่าสุด | `6e6ef9f`, `f9b68e3`, `f0a17d9`, `3df4839` (prefix `alice`) |
| IDF requirement ใน fork | `idf: version: '>=5.5.2'` (**ยังไม่ migrate ไป IDF 6**) |
| esp-sr | `~2.4.6` |
| lvgl | `~9.5.0`, `esp_lvgl_port ~2.8.0` |
| xiaozhi-fonts | `~2.0.0` (ติดตั้งแล้วใน `managed_components/78__xiaozhi-fonts`) |

### 1.2 sdkconfig ที่ใช้จริง
```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_2_06=y
CONFIG_LANGUAGE_TH_TH=y            ← ภาษาไทยเปิดอยู่แล้ว
CONFIG_USE_AFE_WAKE_WORD=y
CONFIG_USE_CUSTOM_WAKE_WORD is not set
CONFIG_USE_AUDIO_PROCESSOR=y
CONFIG_USE_DEVICE_AEC=y            ← hardware AEC (ES8311 + ES7210)
CONFIG_USE_SERVER_AEC is not set
CONFIG_OTA_URL="https://alice.project-alice.uk/xiaozhi/ota/"   ← self-host แล้ว
```

### 1.3 Font pipeline ที่ build จริง (สำคัญมาก — แก้ความเข้าใจผิดเดิม)
`main/CMakeLists.txt:347-351`
```cmake
elseif(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_2_06)
    set(BUILTIN_TEXT_FONT font_noto_sans_basic_30_4)
    set(BUILTIN_ICON_FONT font_material_symbols_30_4)
    set(DEFAULT_EMOJI_COLLECTION alice-moods-64)   ← custom ของเรา
```
`scripts/build_default_assets.py:683-693` + `main/CMakeLists.txt:1225-1228`
> "basic is linked into firmware; **common is loaded from the assets partition**"

แปลว่าเครื่องนี้มี **charset = `common`** (ไม่ใช่ `basic`) เพราะ
`cbin/font_noto_sans_common_30_4.bin` (2.6 MB) ถูก pack ลง assets partition
และ `index.json` ประกาศ `"charset": "common"`

**ผลต่อภาษาไทย:**

| charset | codepoint ทั้งหมด | Thai codepoints |
|---|---|---|
| `basic` (linked in firmware) | 890 | **48** |
| `common` (assets partition) ← **ที่เครื่องนี้ใช้** | 6,845 | **72** |
| `full` (อยู่ฝั่ง server เท่านั้น) | — | ครบ block |

ตัวอักษรไทยที่มีใน `common` (72 ตัว):
```
กขคฆงจฉชซญฎฏฐฑฒณดตถทธนบปผฝพฟภมยรฤลวศษสหอฮฯ
ะัาำิีึืฺุูเแโใไๆ็่้๊๋์ํ๐๑๒๓๔๕
```
ตัวที่ **ขาด** (15 ตัว) — ต้องพึ่ง `glyph_push` หรือ rebuild font:
```
ฃ ฅ ฌ ฦ ฬ ฿ ๅ ๎ ๏ ๖ ๗ ๘ ๙ ๚ ๛
```
> ตัวที่เจ็บจริงมี 2 ตัว: **ฬ** (กีฬา, จุฬา, พาฬ) และ **฿** (สัญลักษณ์บาท)
> ที่เหลือแทบไม่ใช้ในภาษาเขียนปกติ (ฃ ฅ เลิกใช้, ฌ ฦ หายาก, ๖-๙ เลขไทย, ๚ ๛ เครื่องหมายโบราณ)

**สาเหตุที่ขาด (FACT):** `scripts/build.py` ของ xiaozhi-fonts สร้าง charset จาก
(ข้อความใน locale JSON) ∪ (BPE vocab ของ DeepSeek-V4-Flash)
ตัวที่ขาดคือตัวที่ "ไม่มีใครขอ" — ไฟล์ต้นทาง `NotoSansThai-Regular.ttf` **มีครบอยู่แล้ว**
⇒ แก้ได้ด้วยการเติม codepoint แล้ว rebuild ไม่ใช่ข้อจำกัดทางเทคนิค

### 1.4 glyph_push ทำงานอยู่ในเครื่องนี้แล้ว (FACT)
| ไฟล์ | บทบาท |
|---|---|
| `main/assets.h:24` | `bool glyph_push = false;` |
| `main/assets.cc:73,282-294` | ประกาศ capability `glyph_push: true` ให้ server |
| `main/application.cc:573-597` | `TextGlyphPayload::Parse()` → `display->AddTextGlyphs()` |
| `main/display/text_glyph.{h,cc}` | `TextGlyphAllocator` (PSRAM-preferring) |
| `main/display/display.h:47-48` | `AddTextGlyphs()` / `ClearTextGlyphs()` |
| `docs/glyph-push.md` | spec |

**ข้อจำกัด protocol:** ≤64 glyph และ ≤64 KiB ต่อ message, cache ใน PSRAM

⚠️ **กับดักสำคัญ:** `xinnan-tech/xiaozhi-esp32-server` **ไม่ implement glyph_push**
(`grep -rl "glyph_push" srv/` → ไม่พบ) ⇒ ถ้าย้ายไป server ตัวนั้น 15 ตัวที่ขาดจะกลายเป็น
**tofu ถาวร** → ต้องแก้ด้วยวิธี (A) ในข้อ 4.1

### 1.5 Locale ไทย
`main/assets/locales/th-TH/` มีครบ:
- `language.json` — UI strings ไทยเต็ม (`"LISTENING": "กำลังฟัง..."`, `"SPEAKING": "กำลังพูด..."` ฯลฯ)
- เสียงระบบไทย: `0.ogg`–`9.ogg`, `activation.ogg`, `err_pin.ogg`, `err_reg.ogg`, `upgrade.ogg`, `welcome.ogg`, `wificonfig.ogg`

---

## 2. ช่องว่างภาษาไทย 3 ข้อ (วิเคราะห์แล้ว)

### 2.1 Wake word ไทย — **แก้ไม่ได้** (FACT)
`espressif/esp-sr` README (v2.4.x):
> 2026/04/23 — TTS Pipeline V3 รองรับเทรน wake word: **Chinese, English, Japanese, French**
> Planned: Korean, Spanish, Portuguese, German, Russian, Arabic

**ไทยไม่อยู่ทั้งใน list ที่รองรับและ list ที่วางแผน**
โมเดลที่มีจริง: `wn9_hilexin`, `wn10_hilexin`, `wn9_hiesp`, `wn10_hiesp`,
`wn9l_ja_konnichihaesp_tts3`, `wn9l_fr_bonjouresp_tts3`, `wn9_nihaoxiaozhi_tts`,
`wn10_nihaoxiaozhi`, `wn9_xiaoaitongxue`, `wn10_xiaoaitongxue`, `wn9_alexa`, `wn10_alexa`

**สถานะปัจจุบันของเรา:** ใช้ WakeNet9L wake word "TARS" (อังกฤษ) — เป็นทางออกที่ถูกแล้ว
**OPINION:** อย่าเสียเวลาหา wake word ไทย ใช้คำปลุกภาษาอังกฤษต่อไป
(หรือ custom WakeNet ที่เทรนเสียงไทยด้วย phoneme อังกฤษ — ซึ่งทำอยู่แล้ว)

### 2.2 การวางวรรณยุกต์/สระบน — **ต้องทดสอบ** (ASSUMPTION, confidence สูง)
LVGL 9.5 **ไม่มี text shaping engine** (ไม่มี HarfBuzz) → ไม่มี GPOS mark positioning
ภาษาไทยเรียงซ้อน 2 ชั้น เช่น `เปี๊ยก` = ป + ี (สระบน) + ๊ (วรรณยุกต์ชั้น 2)
⇒ คาดว่าวรรณยุกต์ชั้น 2 จะทับสระบน แทนที่จะลอยสูงขึ้น

**ยังไม่พิสูจน์** เพราะยังไม่มี simulator → นี่คือเหตุผลที่งาน SDL2 sim (ข้อ 4.2) มาก่อน

### 2.3 Cloud ASR/TTS จีน — **แก้ไปแล้วบางส่วน** (FACT)
`xiaozhi.me` default ใช้ pipeline ที่ optimize ให้ภาษาจีน
เรา self-host ที่ `alice.project-alice.uk` อยู่แล้ว + มี Google STT ไทย (15 phrase hints)
+ Minimax TTS (TARS/Aqua voice clone) + Sonnet 5 function_call ผ่าน Anthropic direct

---

## 3. สิ่งที่ดึงมาจากโปรเจคอื่น (research แล้ว)

### 3.1 `xinnan-tech/xiaozhi-esp32-server` (⭐10,633, push 2026-09-20)
**ASR providers ที่มี** (`main/xiaozhi-server/core/providers/asr/`):
`aliyun`, `aliyun_stream`, `aliyunbl_stream`, `baidu`, `doubao`, `doubao_stream`,
`fun_local`, `fun_server`, **`openai`**, `qwen3_asr_flash`, `sherpa_onnx_local`,
`tencent`, `vosk`, `xunfei_stream`

**TTS providers ที่มี** (`.../tts/`):
`alibl_stream`, `aliyun`, `aliyun_stream`, `cozecn`, `custom`, `default`, `doubao`,
**`edge`**, `fishspeech`, `gpt_sovits_v2`, `gpt_sovits_v3`, `huoshan_double_stream`,
`index_stream`, **`minimax_httpstream`**, `openai`, `paddle_speech`, `siliconflow`,
`tencent`, `xunfei_stream`

**ของที่เอามาใช้ได้ทันที:**
- `tts/minimax_httpstream.py` — มีอยู่แล้ว ⇒ voice clone TARS/Aqua เสียบตรงได้
- `tts/edge.py` — voice เป็น config ล้วน (`config.get("private_voice")` / `config.get("voice")`)
  ⇒ ใส่ `th-TH-PremwadeeNeural` / `th-TH-NiwatNeural` ได้ฟรี
- `asr/openai.py` — Whisper-compatible ⇒ ชี้ไป Groq `whisper-large-v3-turbo` ได้

**⚠️ bug ที่ต้อง patch เอง** — `asr/openai.py` ส่งแค่ model ไม่ส่ง language:
```python
data = {"model": self.model}          # ← ขาด language, ขาด prompt
with open(file_path, "rb") as audio_file:
    files = {"file": audio_file}
    response = requests.post(self.api_url, files=files, data=data, headers=headers)
```
ต้องเติม `data["language"] = "th"` + `data["prompt"] = "<phrase hints>"`

**สิ่งที่ขาด:** `grep -ril "th-TH\|thai"` ใน `main/xiaozhi-server` → **ไม่พบ** (ไม่มี Thai preset)
และ `grep -rl "glyph_push"` → **ไม่พบ** (ดูข้อ 1.4)

**Deploy:** `docker-compose.yml` = container เดียว
(`ghcr.nju.edu.cn/xinnan-tech/xiaozhi-esp32-server:server_latest`),
port `8000` (WebSocket) + `8003` (HTTP OTA/vision), volume `./data` + `SenseVoiceSmall/model.pt`
(`docker-compose_all.yml` เพิ่ม MySQL/Redis/Java manager-api/web — ไม่จำเป็น)

### 3.2 `akdeb/ElatoAI` (⭐2,016, push 2026-09-02)
**อัปเดต 2026:** Apr 17 Cloudflare Voice Agents + Durable Objects ·
Apr 15 FastAPI/Pipecat server (100+ STT/LLM/TTS pipeline) ·
Mar 14 Local AI Toys (Qwen/Mistral on-device ผ่าน MLX)

**เอามา 3 อย่าง:**
1. **Realtime speech-to-speech** — ตัด STT→LLM→TTS เหลือ hop เดียว (แนะนำ Gemini Live สำหรับไทย)
2. **Edge-runtime pattern** (Cloudflare Workers + Durable Objects) — ตรงกับ stack ที่เรามีอยู่
3. **MLX local-model pattern** — ตรงกับ MLX server บน M1 ที่รันอยู่ (Qwen3.5-4B, 21.3 tok/s)

**❌ อย่าทำ:** อย่าย้ายไป firmware ของ ElatoAI (Arduino) — จะเสีย LVGL UI, mood avatar,
รองรับ 138 บอร์ด และ glyph_push ทั้งหมด

**⚠️ ความเสี่ยงทางวิศวกรรม:** firmware ส่ง **Opus** แต่ Realtime API ต้องการ **PCM16/G.711**
⇒ ต้องมี transcode bridge (ง่ายบน Mac ด้วย `opuslib`, ต้องใช้ WASM libopus ถ้าอยู่บน Workers)

### 3.3 `andygeiss/esp32-watch` (⭐1, 47 commits, MIT)
เล็กมากแต่มีของดี 1 อย่างที่ **ตรงปัญหาเราที่สุด**:
> **SDL2 host simulator รันที่ความละเอียดจริง 410×502 + headless golden-image renderer validation**

⇒ ใช้ debug การวางวรรณยุกต์ไทยบน Mac ได้โดยไม่ต้อง flash ทีละรอบ
⇒ ทำ regression test แบบเทียบภาพได้

### 3.4 โปรเจคอื่นที่สำรวจแล้ว (อ้างอิง)
| repo | ⭐ | push ล่าสุด | สรุป |
|---|---|---|---|
| 78/xiaozhi-esp32 | 30,089 | 2026-09-20 | upstream ของเรา |
| xinnan-tech/xiaozhi-esp32-server | 10,633 | 2026-09-20 | server ทางเลือก |
| HeyWillow/willow (ย้ายจาก toverainc) | 3,105 | 2026-09-01 | local voice, ไม่มีไทย |
| espressif/esp-adf | 2,310 | 2026-09-10 | audio framework |
| akdeb/ElatoAI | 2,016 | 2026-09-02 | ดูข้อ 3.2 |
| espressif/esp-brookesia | 793 | 2026-09-18 | UI framework |
| wangzongming/esp-ai | 855 | 2026-01-09 | **stale** |
| openai/openai-realtime-embedded | 1,576 | 2025-03-25 | **stale** |
| waveshareteam/ESP32-AIChats | 97 | 2026-08-07 | official demo |
| kaloprojects/KALO-ESP32-Voice-Chat-AI-Friends | 75 | 2026-09-18 | Arduino, เล็ก |

**เฉพาะบอร์ด AMOLED 2.06:**
infinition/waveshare-watch-rs (372⭐ Rust no_std) ·
waveshareteam/ESP32-S3-Touch-AMOLED-2.06 (115⭐ official BSP — clone อยู่ที่ `~/ESP32-S3-Touch-AMOLED-2.06/`) ·
MarcoRR/S3NTRY (56⭐) · joaquimorg/OLEDS3Watch (31⭐ ESP-Brookesia) ·
LostBeard/SpawnWear (9⭐ .NET nanoFramework) · niclasvestlund-YT/vibepulse (200⭐)

### 3.5 Upstream release ที่ยังไม่ได้ merge
| version | วันที่ | ของสำคัญ |
|---|---|---|
| v2.5.0 | 2026-09-10 | WK ESP32S3 + FoloToy board, streamed notification playback, locale mr-IN + pt-BR, OLED idle power-save |
| v2.4.2 | 2026-08-06 | firmware-builder (xiaozhi.me/console/firmware-builder), จัด board ตาม manufacturer, ลบ acoustic provisioning |
| v2.4.0 | 2026-07-19 | **ย้ายไป ESP-IDF 6.0**, migrate ไป Noto font |

⚠️ upstream ปัจจุบันบังคับ `idf: >=6.0.1` แต่ fork เราอยู่ `>=5.5.2`
⇒ ถ้าจะ merge v2.4.0+ ต้อง migrate IDF ก่อน (มี `docs/esp-idf-6-migration.md` ใน repo แล้ว)
⇒ ถ้ายังไม่พร้อม ให้ pin ที่ v2.4.2 หรือเก่ากว่า

---

## 4. แผนงาน 6 ขั้น (เรียงตาม ROI)

### ขั้นที่ 1 — เติม charset ไทยให้ครบ block แล้ว rebuild font · ~1 ชม. · **ทำก่อน**
**ทำไม:** ปิดช่องโหว่ `ฬ` และ `฿` ถาวร และทำให้ไม่ต้องพึ่ง glyph_push สำหรับภาษาไทยเลย
(สำคัญมากถ้าจะย้ายไป xiaozhi-esp32-server ซึ่งไม่มี glyph_push)

**ทำอะไร:**
1. เติม codepoint `0x0E01`–`0x0E5B` ทั้ง block ลง
   `managed_components/78__xiaozhi-fonts/charsets/common-requested.json`
2. รัน `scripts/build.py` ของ xiaozhi-fonts (ต้องมี `requirements.txt` ติดตั้งก่อน)
3. rebuild → `cbin/font_noto_sans_common_30_4.bin` ใหม่
4. `idf.py build` แล้วเช็คขนาด assets partition

**ต้นทุน:** ~5 KB flash เพิ่ม ที่ profile `20_4` (profile `30_4` จะมากกว่านี้ — วัดจริงตอนทำ)
**เสี่ยง:** ต่ำ. ถ้า component ถูก re-download จาก registry การแก้จะหาย
⇒ **ควร vendor เข้า repo** (คัดลอกไป `components/xiaozhi-fonts/` แล้วถอดออกจาก `idf_component.yml`)

### ขั้นที่ 2 — port SDL2 host simulator (410×502) · ~3–4 ชม. · **คุ้มที่สุด**
**ทำไม:** โดยไม่มีตัวนี้ การ debug การวาง mark ไทยต้อง flash ทุกรอบ (~2 นาที/รอบ)
มีแล้วเหลือ ~2 วินาที/รอบ + ทำ golden-image regression test ได้

**ทำอะไร:**
1. ดูวิธีของ `andygeiss/esp32-watch` (SDL2 + LVGL host build)
2. สร้าง `sim/` target: CMake แยก, link LVGL host + SDL2, ตั้ง `LV_HOR_RES=410` `LV_VER_RES=502`
3. stub ชั้น hardware (audio codec, touch, WiFi) ให้ compile ผ่าน
4. เพิ่ม headless renderer → PNG เพื่อเทียบ golden image
5. ทดสอบ string ไทยชุดโหด: `เปี๊ยก น้ำ ที่ ผู้ ปุ๋ย กีฬา ก็ ฿1,200 ๆ`

**ผลลัพธ์:** ได้คำตอบชี้ขาดว่า ASSUMPTION 2.2 จริงหรือไม่

### ขั้นที่ 3 — patch `asr/openai.py` ให้ส่ง language + prompt · ~20 นาที
เฉพาะกรณีที่จะใช้ xiaozhi-esp32-server. เติม:
```python
data = {"model": self.model, "language": "th"}
if self.prompt:                       # phrase hints ไทย 15 ตัวที่มีอยู่แล้วจาก Google STT
    data["prompt"] = self.prompt
```
**ผลที่คาด (ASSUMPTION):** WER ภาษาไทยลดลงชัดเจน เพราะ Whisper auto-detect
มักสลับไป lang อื่นเมื่อเจอเสียงสั้น/ปนอังกฤษ

### ขั้นที่ 4 — ตัดสินใจเรื่อง hosting · ~2 ชม.
**สถานะ (FACT):** ตอนนี้ self-host แล้วที่ `alice.project-alice.uk` (Cloudflare Tunnel → Mac M1)

**Lightning.ai — ข้อมูลที่ยืนยันจาก official docs:**
- ฟรี: **1 Studio 4-CPU**, Drive 10 GB แรกฟรี, เพดาน free tier 50 GB, เกิน 10 GB คิด $0.10/GB/เดือน
- เปิด public port ได้ผ่าน **API builder plugin** (รองรับ token auth / basic auth)
- "Studios support any kind of server as long as it exposes a port"; หลาย server ใน Studio เดียว = load balancer
- Serverless: "the Studio will turn off if the app is not being used and will turn on again if it is being used"
- Free credits เป็น promotional, ไม่สะสม, Lightning เปลี่ยน/จำกัดได้ทุกเมื่อ

**ยังไม่ยืนยัน (UNVERIFIED):** auto-sleep 10 นาทีเมื่อ idle และการขยาย/ปิด auto-sleep
ต้องใช้ Lightning **Pro** — ทั้ง `/docs/overview/ai-studio/auto-sleep` และ
`/docs/platform/build/ai-studio/auto-sleep` ตอบ **404** ยังหาหน้าที่ถูกไม่เจอ

**ยังไม่รู้:** latency จากไทยไป region ของ Lightning · เนื้อหาใน teamspace
`lightning.ai/attydhamanoon-org/general/studios` (**private — 404 ถ้าไม่ล็อกอิน**)

**OPINION / คำแนะนำ:**
- **primary host = Mac M1 + Cloudflare Tunnel เดิม** (ที่ใช้อยู่) — อุปกรณ์เสียง always-listening
  ทนกับ cold start ไม่ได้ ถ้า auto-sleep 10 นาทีจริง Lightning จะตกรอบทันที
- **Lightning.ai = บทบาทรอง** — งาน GPU on-demand (เทรน Thai TTS, host LLM ใหญ่ชั่วคราว,
  batch job) ที่ยอมรับ cold start ได้ ไม่ใช่ตัวรับ WebSocket ของอุปกรณ์
- **ต้องถามผู้ใช้:** plan tier (Free/Pro), machine type, credits ที่เหลือ, region

### ขั้นที่ 5 — แก้การวางวรรณยุกต์ไทย (ถ้าขั้นที่ 2 ยืนยันว่าพัง) · ~1–2 วัน
**⚠️ ต้องให้ผู้ใช้เลือก policy ก่อนลงมือ — ยังไม่ได้คำตอบ:**

| ทางเลือก | ข้อดี | ข้อเสีย |
|---|---|---|
| **(1) shift-up** เลื่อน mark ชั้น 2 ขึ้นตามความสูงสระบน | ถูกต้องตามหลักภาษา, ครอบคลุมทุกคู่ | กิน line-height บนจอ 502 px |
| **(2) precomposed glyph pairs** ทำ glyph รวมสำเร็จรูป | เร็วที่สุด, สวยที่สุด | ต้องมี lookup table, coverage ไม่ครบ |
| **(3) ยอมให้ทับ** ไม่ทำอะไร | ต้นทุน 0 | อ่านผิดความหมายได้ (ที่ ↔ ที่) |

**เตรียมไว้ให้:** จะ scaffold `main/display/thai_shaper.cc` พร้อม signature
`ResolveMarkOffset()` (~8 บรรทัดให้ผู้ใช้เขียนเอง) + test fixture

### ขั้นที่ 6 — Gemini Live realtime bridge (optional) · ~3–5 วัน
ตัด latency STT→LLM→TTS ให้เหลือ hop เดียว
**ติดขัดหลัก:** firmware ส่ง Opus / API ต้องการ PCM16 ⇒ ต้องเขียน transcode bridge
บน Mac ใช้ `opuslib` ได้เลย บน Cloudflare Workers ต้อง WASM libopus

---

## 5. คำถามที่ยังค้าง (ต้องได้คำตอบก่อนเดินต่อบางขั้น)

1. **Lightning.ai** — plan tier อะไร? machine type? credits เหลือเท่าไร? region ไหน?
   (ผมเปิด teamspace ไม่ได้ — private, browser pane ไม่มี session ของคุณ)
2. **Thai mark policy** — เลือก (1) shift-up / (2) precomposed / (3) ยอมทับ?
3. **จะ migrate ไป ESP-IDF 6.0 ไหม?** ถ้าใช่จึงจะ merge upstream v2.4.0+ ได้
4. **จะเริ่มจากขั้นไหน?** (แนะนำ: ขั้นที่ 1 → ขั้นที่ 2)

---

## 6. คำสั่งที่ใช้ตรวจสอบซ้ำได้

```bash
# ยืนยัน config ของบอร์ด
grep -E "CONFIG_BOARD_TYPE.*=y|CONFIG_LANGUAGE.*=y|CONFIG_USE_DEVICE_AEC|CONFIG_OTA_URL" \
  ~/xiaozhi-esp32/sdkconfig
```

```bash
# ยืนยันว่า font ที่ build คือ basic_30_4 + common cbin
grep -n -A4 "AMOLED_2_06)" ~/xiaozhi-esp32/main/CMakeLists.txt | head -20
```

```bash
# นับ codepoint ไทยใน charset แต่ละชั้น
python3 -c "
import json
for n in ['basic','common']:
    cps=json.load(open(f'$HOME/xiaozhi-esp32/managed_components/78__xiaozhi-fonts/charsets/{n}.json'))
    cps=cps if isinstance(cps,list) else cps.get('codepoints',[])
    th=[c for c in cps if 0x0E00<=c<=0x0E7F]
    print(n, len(cps), 'total /', len(th), 'thai:', ''.join(chr(c) for c in sorted(th)))
"
```

```bash
# ยืนยันว่า glyph_push client มีจริงใน firmware
grep -rn "glyph_push\|TextGlyph" ~/xiaozhi-esp32/main | head -20
```

---

## 7. บันทึกความผิดพลาดของ research รอบนี้ (กันทำซ้ำ)

- **อย่า grep หาอักขระไทยตรงๆ ใน `charsets/*.json`** — ไฟล์เก็บเป็น **integer codepoint** ไม่ใช่ตัวอักษร
  ต้อง parse JSON แล้วกรอง `0x0E00 <= cp <= 0x0E7F`
- **GitHub Code Search API ตอบ 401** ("Requires authentication") ⇒ ดาวน์โหลด tarball มา grep เองแทน
- **`api.github.com/repos/...` บาง repo ตอบ "Moved Permanently"** ⇒ ใช้ `curl -sL` ตาม redirect
  (`openai-realtime-embedded-sdk` → `openai-realtime-embedded`, `toverainc/willow` → `HeyWillow/willow`)
- **WebFetch ใช้กับ lightning.ai ไม่ได้** (SPA render ฝั่ง client, คืนแค่ "Taking longer than expected")
  ⇒ ใช้ browser pane: `navigate` → `wait 4-5s` → `get_page_text`
- **zsh กิน glob** — ต้อง quote `--include="*.cc"` เวลาใช้ grep ผ่าน Bash tool
