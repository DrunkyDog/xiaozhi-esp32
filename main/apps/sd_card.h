/*
 * SdCard — mount microSD บน ESP32-S3-Touch-AMOLED 2.06
 *
 * Hardware (จาก Hardware-Pinout.md): microSD ต่อผ่าน SPI3 แบบ 1-bit
 *   SD_CLK  = GPIO2      SD_CMD (MOSI) = GPIO1
 *   SD_DATA (MISO) = GPIO3   SD_CS = GPIO17
 * ตรวจแล้วว่า GPIO 1/2/3/17 ไม่ชนกับอะไรบนบอร์ดนี้ และจอใช้ SPI2_HOST
 * (SPI3 จึงว่าง)
 *
 * ⚠️ ข้อจำกัดจาก sdkconfig ปัจจุบัน:
 *   CONFIG_FATFS_LFN_NONE=y  → ชื่อไฟล์ต้องเป็น 8.3 เท่านั้น (CSI00001.CSV)
 *                              ชื่อยาวจะสร้างไม่สำเร็จแบบเงียบ ๆ
 *   CONFIG_FATFS_CODEPAGE_437 → ชื่อไฟล์ภาษาไทยไม่ได้
 *   การ์ดต้องเป็น FAT32 + MBR (ESP-IDF ปิด exFAT: FF_FS_EXFAT 0)
 */
#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>
#include <sdmmc_cmd.h>

#define SD_MOUNT_POINT  "/sdcard"

// pinout ของบอร์ด 2.06
#define SD_PIN_CLK   GPIO_NUM_2
#define SD_PIN_MOSI  GPIO_NUM_1
#define SD_PIN_MISO  GPIO_NUM_3
#define SD_PIN_CS    GPIO_NUM_17

class SdCard {
public:
    static SdCard& GetInstance();

    bool Mount();                     // idempotent — เรียกซ้ำได้
    void Unmount();
    bool IsMounted() const { return mounted_; }

    uint64_t TotalBytes() const;
    uint64_t FreeBytes() const;
    const char* MountPoint() const { return SD_MOUNT_POINT; }

private:
    SdCard() = default;
    sdmmc_card_t* card_    = nullptr;
    bool          mounted_ = false;
    bool          bus_init_ = false;
};

#endif // SD_CARD_H
