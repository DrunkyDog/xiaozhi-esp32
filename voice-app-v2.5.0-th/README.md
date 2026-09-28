# AI Voice — xiaozhi upstream v2.5.0 (ภาษาไทย)

บอร์ด: **Waveshare ESP32-S3-Touch-AMOLED-2.06** เท่านั้น · flash layout 16 MB (`partitions/v2/16m.csv`, ใช้กับชิป 32 MB ได้)
Build: 2026-09-28 13:42 · ESP-IDF v6.0.3 · source = tag `v2.5.0` (ac6deed) + `firmware.patch` (ธีมมืด + ใช้ esp-wifi-connect ฉบับ local) + `wifi-page-no-chinese.patch` + `ota-hardening.patch` — **ไม่มี** radar / flight radar / CSI / AppManager / persona / mood / motor ของ Alice

| offset | ไฟล์ |
|---|---|
| 0x000000 | `0x000000_bootloader.bin` |
| 0x008000 | `0x008000_partition-table.bin` (nvs 0x9000 · otadata 0xd000 · phy_init 0xf000 · ota_0 0x20000 · ota_1 0x410000 · assets 0x800000) |
| 0x00d000 | `0x00d000_ota_data_initial.bin` |
| 0x020000 | `0x020000_xiaozhi.bin` (app 2.5.0 + ota-hardening) |
| 0x800000 | `0x800000_generated_assets.bin` (font noto_sans_common_30 · srmodels = WakeNet9 `computer` · emoji noto 64px) |
| 0x000000 | `voice-app-v2.5.0-th_merged_0x0.bin` — รวมทุกไฟล์ข้างบนเป็นไฟล์เดียว (11.4 MB) |

Config (`sdkconfig.defaults.voice-th`): ไทย · OTA `https://alice.project-alice.uk/xiaozhi/ota/` · device AEC · wake word "computer" · ธีมมืดเป็นค่าเริ่มต้น · หน้าตั้งค่า WiFi ไม่มีภาษาจีน (ลบ zh-CN/zh-TW, เบราว์เซอร์ภาษาจีนจะเห็น English)

```bash
./flash.sh /dev/cu.usbmodemXXXX              # ทั้งชุด — เก็บ WiFi/ค่าเดิม (NVS) ไว้
./flash.sh /dev/cu.usbmodemXXXX --erase-nvs  # ทั้งชุด + ล้าง WiFi/ค่าเดิม
./flash.sh /dev/cu.usbmodemXXXX --app-only   # เฉพาะ app + รีเซ็ต otadata ให้บูต ota_0 (เครื่องต้องใช้ layout 16MB v2 อยู่แล้ว)
./flash.sh /dev/cu.usbmodemXXXX --merged     # ไฟล์เดียวที่ 0x0 — ล้าง NVS และ ota_1 ด้วย (factory reset)
```

ใช้ไฟล์ merged กับเครื่องมืออื่น (เช่น ESP Web Flasher, Flash Download Tool) ได้ที่ offset `0x0` · ตรวจไฟล์ก่อน flash ด้วย `shasum -a 256 -c SHA256SUMS`

## ota-hardening.patch (`main/ota.cc`)

- **ตรวจ SHA-256 ของ firmware:** ถ้า OTA server ส่ง `"sha256": "<64 hex>"` มาใน `firmware` จะตรวจก่อนสลับช่องบูต ไม่ตรงก็ยกเลิก ถ้าไม่ส่งมาก็ทำงานแบบเดิม
- **กัน HTTP ไม่มีการยืนยัน:** URL firmware แบบ `http://` จะรับเฉพาะเมื่อมี `sha256` จาก OTA server มาด้วย
- **ตรวจขนาดที่ดาวน์โหลด:** ต้องเท่ากับ Content-Length
- **แก้ `ParseVersion`:** เดิมใช้ `std::stoi` ขณะที่ build นี้ปิด C++ exceptions ถ้า server ส่งเวอร์ชันแบบ `v2.6.0` เครื่องจะ `abort()` ตอนนี้เปลี่ยนเป็น `strtol` แทน

ตัวอย่าง response จาก OTA server:
```json
{ "firmware": { "version": "2.5.1", "url": "https://host/xiaozhi.bin", "sha256": "…" } }
```

Build ซ้ำ: worktree `.worktrees/voice-v2.5.0` →
`idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.voice-th" set-target esp32s3 build`
