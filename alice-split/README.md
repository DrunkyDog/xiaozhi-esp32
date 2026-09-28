# alice-split

แยก `alice-merged.bin` (Waveshare ESP32-S3-Touch-AMOLED-2.06 · xiaozhi · IDF v6.0.3) กลับเป็นชิ้นส่วน
และจัด source ที่ compile เข้า `xiaozhi.bin` จริงออกเป็นกลุ่ม

| โฟลเดอร์ | เนื้อหา |
|---|---|
| `00_merged/` | `alice-merged.bin` สร้างด้วย `esptool merge-bin` จาก `build/` (ตาม `flasher_args.json`) |
| `01_flash_images/` | ตัดออกจาก merged ตาม partition table + ESP image header — ตรงกับ `build/` ทุก byte |
| `02_assets_unpacked/` | 24 ไฟล์จาก `generated_assets.bin` (ตรวจ checksum แล้ว) |
| `03_source_by_group/` | 01_Core · 02_Board · 03_Audio · 04_Protocol · 05_Display · 06_Apps · 07_Embedded_ogg · 08_Components · 09_Build_config |
| `MANIFEST.json` | offset/size/sha256 ของ image, รายการ source ต่อกลุ่ม (`compiled: true` = อยู่ใน compile_commands.json) |
| `alice-code-map.html` | system diagram · boot flow chart · sequence เสียง/MCP · ตารางเรียกใช้/ถูกเรียกใช้ |

รันซ้ำ (หลัง build ใหม่):

```bash
cd build && esptool --chip esp32s3 merge-bin -o ../alice-split/00_merged/alice-merged.bin \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
  0xd000 ota_data_initial.bin 0x20000 xiaozhi.bin 0x800000 generated_assets.bin
cd .. && python3 alice-split/tools/split_and_group.py
```

หมายเหตุ: source ใน `03_source_by_group/` เป็น **สำเนา** ณ เวลาที่รัน (working tree มีไฟล์ที่ยังไม่ commit) —
แก้โค้ดที่ `main/` เสมอ ไม่ใช่ที่นี่ · `08_Components/` copy เฉพาะ `local_components/` ส่วน managed_components (723 MB)
เก็บเป็นรายการ + version ใน `linked_components.json`
