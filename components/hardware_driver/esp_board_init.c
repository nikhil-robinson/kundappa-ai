/**
 * 
 * @copyright Copyright 2021 Espressif Systems (Shanghai) Co. Ltd.
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *               http://www.apache.org/licenses/LICENSE-2.0

 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_board_init.h"

static const char *TAG = "hardware";

static esp_codec_dev_handle_t spk_codec_dev = NULL;
static esp_codec_dev_handle_t mic_codec_dev = NULL;

esp_err_t esp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    esp_err_t err = ESP_OK;
    spk_codec_dev = bsp_audio_codec_speaker_init();
    assert(spk_codec_dev);
    mic_codec_dev = bsp_audio_codec_microphone_init();
    assert(mic_codec_dev);



    err |= esp_codec_dev_set_out_vol(spk_codec_dev, 100);
    err |= esp_codec_dev_set_in_gain(mic_codec_dev, 50.0);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = channel_format,
        .bits_per_sample = bits_per_chan,
    };
    err |=esp_codec_dev_open(spk_codec_dev, &fs);
    err |=esp_codec_dev_open(mic_codec_dev, &fs);
    return err;
}

esp_err_t esp_sdcard_init(char *mount_point, size_t max_files)
{
    return bsp_sdcard_init(mount_point, max_files);
}

esp_err_t esp_sdcard_deinit(char *mount_point)
{
    return bsp_sdcard_deinit(mount_point);
}

esp_err_t esp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    // return bsp_get_feed_data(is_get_raw_channel, buffer, buffer_len);
    // return esp_codec_dev_read(mic_codec_dev, buffer, buffer_len);
    esp_err_t ret = ESP_OK;
    size_t bytes_read;
    int audio_chunksize = buffer_len / (sizeof(int16_t) * 4);

    ret = esp_codec_dev_read(mic_codec_dev, (void *)buffer, buffer_len);
    if (!is_get_raw_channel) {
        for (int i = 0; i < audio_chunksize; i++) {
            int16_t ref = buffer[4 * i + 0];
            buffer[3 * i + 0] = buffer[4 * i + 1];
            buffer[3 * i + 1] = buffer[4 * i + 3];
            buffer[3 * i + 2] = ref;
        }
    }

    return ret;

}

int esp_get_feed_channel(void)
{
    return 4;
}

esp_err_t esp_audio_play(const int16_t* data, int length, TickType_t ticks_to_wait)
{
    // return bsp_audio_play(data, length, ticks_to_wait);
    return esp_codec_dev_write(spk_codec_dev, data, length);
}

esp_err_t esp_audio_set_play_vol(int volume)
{
    // return bsp_audio_set_play_vol(volume);
    return esp_codec_dev_set_out_vol(spk_codec_dev, volume);
}

esp_err_t esp_audio_get_play_vol(int *volume)
{
    // return bsp_audio_get_play_vol(volume);
    return esp_codec_dev_get_out_vol(spk_codec_dev,volume);
}

esp_err_t FatfsComboWrite(const void* buffer, int size, int count, FILE* stream)
{
    esp_err_t res = ESP_OK;
    res = fwrite(buffer, size, count, stream);
    res |= fflush(stream);
    res |= fsync(fileno(stream));

    return res;
}