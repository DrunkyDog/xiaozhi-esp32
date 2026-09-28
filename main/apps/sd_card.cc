#include "sd_card.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <sys/stat.h>

#define TAG "SdCard"

SdCard& SdCard::GetInstance() {
    static SdCard inst;
    return inst;
}

bool SdCard::Mount() {
    if (mounted_) return true;

    // SPI3 ว่าง — จอบอร์ดนี้ใช้ SPI2_HOST
    if (!bus_init_) {
        spi_bus_config_t bus = {};
        bus.mosi_io_num     = SD_PIN_MOSI;
        bus.miso_io_num     = SD_PIN_MISO;
        bus.sclk_io_num     = SD_PIN_CLK;
        bus.quadwp_io_num   = -1;
        bus.quadhd_io_num   = -1;
        bus.max_transfer_sz = 4096;

        esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = init ไว้แล้ว
            ESP_LOGE(TAG, "spi_bus_initialize ล้มเหลว: %s", esp_err_to_name(err));
            return false;
        }
        bus_init_ = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 20000;              // 20MHz — ปลอดภัยกับสายบนบอร์ด

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;    // ❗ห้าม format การ์ดของ user เองเด็ดขาด
    mcfg.max_files              = 4;
    mcfg.allocation_unit_size   = 16 * 1024;

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &card_);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "mount ไม่สำเร็จ — การ์ดต้องเป็น FAT32 + MBR "
                          "(ESP-IDF ไม่รองรับ exFAT)");
        } else {
            ESP_LOGE(TAG, "mount ล้มเหลว: %s (เช็คว่าเสียบการ์ดแล้วหรือยัง)",
                     esp_err_to_name(err));
        }
        card_ = nullptr;
        return false;
    }

    mounted_ = true;
    ESP_LOGI(TAG, "mounted ที่ %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, card_);
    // ⚠️ CONFIG_NEWLIB_NANO_FORMAT=y → nano printf ไม่รองรับ %llu/%f
    // แปลงเป็น MB ก่อน (32GB = 30518 MB พอดีกับ 32-bit)
    ESP_LOGI(TAG, "ความจุ %u MB / ว่าง %u MB",
             (unsigned)(TotalBytes() / (1024 * 1024)),
             (unsigned)(FreeBytes()  / (1024 * 1024)));
    return true;
}

void SdCard::Unmount() {
    if (!mounted_) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card_);
    card_ = nullptr;
    mounted_ = false;
    ESP_LOGI(TAG, "unmounted");
}

uint64_t SdCard::TotalBytes() const {
    if (!mounted_ || !card_) return 0;
    return (uint64_t)card_->csd.capacity * card_->csd.sector_size;
}

uint64_t SdCard::FreeBytes() const {
    if (!mounted_) return 0;
    FATFS* fs;
    DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) return 0;
    // ❗ต้องใช้ fs->ssize (sector ของ SD = 512) ไม่ใช่ CONFIG_WL_SECTOR_SIZE (4096
    // ซึ่งเป็นของ wear-levelling บน SPI flash) — ใช้ผิดจะได้ค่าใหญ่เกิน 8 เท่า
    return (uint64_t)free_clusters * fs->csize * fs->ssize;
}
