#!/usr/bin/env bash
# AI Voice (xiaozhi upstream v2.5.0, Thai) — Waveshare ESP32-S3-Touch-AMOLED-2.06
# usage: ./flash.sh <port>              full flash, keeps NVS (Wi-Fi/settings)
#        ./flash.sh <port> --erase-nvs  full flash + erase NVS (Wi-Fi/settings reset)
#        ./flash.sh <port> --app-only   app at 0x20000 + otadata reset so ota_0 boots
#                                       (device must already use the 16MB v2 layout)
#        ./flash.sh <port> --merged     single image at 0x0 (factory reset: NVS and ota_1 are wiped)
set -euo pipefail
PORT="${1:?usage: $0 <serial-port> [--erase-nvs|--app-only|--merged]}"
MODE="${2:-}"
cd "$(dirname "$0")"
shasum -a 256 -c SHA256SUMS

ESPTOOL=(esptool --chip esp32s3 -p "$PORT" -b 921600)
FLASH_OPTS=(--flash-mode dio --flash-size 16MB --flash-freq 80m)

case "$MODE" in
  "" | --erase-nvs)
    if [ "$MODE" = "--erase-nvs" ]; then
      "${ESPTOOL[@]}" erase-region 0x9000 0x4000
    fi
    "${ESPTOOL[@]}" write-flash "${FLASH_OPTS[@]}" \
      0x0 0x000000_bootloader.bin 0x8000 0x008000_partition-table.bin 0xd000 0x00d000_ota_data_initial.bin \
      0x20000 0x020000_xiaozhi.bin 0x800000 0x800000_generated_assets.bin
    ;;
  --app-only)
    # After an OTA the device may boot from ota_1; resetting otadata makes the new ota_0 image boot
    "${ESPTOOL[@]}" write-flash 0xd000 0x00d000_ota_data_initial.bin 0x20000 0x020000_xiaozhi.bin
    ;;
  --merged)
    "${ESPTOOL[@]}" write-flash "${FLASH_OPTS[@]}" 0x0 voice-app-v2.5.0-th_merged_0x0.bin
    ;;
  *)
    echo "unknown option: $MODE" >&2
    exit 2
    ;;
esac
