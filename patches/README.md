# Local patches for managed_components

`managed_components/` is gitignored and rewritten whenever the component
manager re-resolves dependencies (manifest change, `dependencies.lock`
removal, component update). Patches applied there are lost silently, so
they are kept here and re-applied by hand.

## esp_codec_dev-i2s-spurious-disable.patch

`espressif/esp_codec_dev` @ `platform/audio_codec_data_i2s.c` calls
`_i2s_drv_enable(paired, true, false)` inside a branch whose condition
already guarantees `paired->out_enable == false`. The channel is not
enabled, so `i2s_channel_disable()` always fails and the driver logs

    E i2s_common: i2s_channel_disable(1370): the channel has not been enabled yet

on every alert. The return value is discarded and out_enable/in_enable are
only written in `_i2s_data_enable()`, so dropping the call is
behaviour-preserving: it removes the spurious ESP_LOGE and nothing else.

Verified on esp32-s3-touch-amoled-2.06: occurrences went from 3 per
3 minutes to 0, audio and MotorDiag unaffected.

Re-apply after any dependency re-resolve:

    patch -p0 < patches/esp_codec_dev-i2s-spurious-disable.patch
