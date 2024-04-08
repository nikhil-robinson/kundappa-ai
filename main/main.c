/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "model_path.h"
#include "esp_process_sdkconfig.h"

int detect_flag = 0;
static esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;
srmodel_list_t *models = NULL;
static int play_voice = -2;

void play_music(void *arg)
{
    while (task_flag) {
        switch (play_voice) {
        case -2:
            vTaskDelay(10);
            break;
        case -1:
            wake_up_action();
            play_voice = -2;
            break;
        default:
            speech_commands_action(play_voice);
            play_voice = -2;
            break;
        }
    }
    vTaskDelete(NULL);
}

void feed_Task(void *arg)
{
    ESP_LOGI(TAG, "Feed Task");
    size_t bytes_read = 0;
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *) arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel = 3;
    ESP_LOGI(TAG, "audio_chunksize=%d, feed_channel=%d", audio_chunksize, feed_channel);

    int16_t *audio_buffer = heap_caps_malloc(audio_chunksize * sizeof(int16_t) * feed_channel, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(audio_buffer);

    while (true) {
        esp_get_feed_data(false, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);
        bsp_i2s_read((char *)audio_buffer, audio_chunksize * I2S_CHANNEL_NUM * sizeof(int16_t), &bytes_read, portMAX_DELAY);

        for (int  i = audio_chunksize - 1; i >= 0; i--) {
            audio_buffer[i * 3 + 2] = 0;
            audio_buffer[i * 3 + 1] = audio_buffer[i * 2 + 1];
            audio_buffer[i * 3 + 0] = audio_buffer[i * 2 + 0];
        } 

        afe_handle->feed(afe_data, audio_buffer);
    }
    if (audio_buffer) {
        free(audio_buffer);
        audio_buffer = NULL;
    }
    vTaskDelete(NULL);
}

void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);


    printf("multinet:%s\n", mn_name);
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);


    esp_mn_commands_clear();                       // Clear commands that already exist 
    esp_mn_commands_add(1, "Hi Kndappa");   // add a command
    esp_mn_commands_add(2, "Turn on the light");  // add a command
    esp_mn_commands_update();                      // update commands


    int mu_chunksize = multinet->get_samp_chunksize(model_data);
    assert(mu_chunksize == afe_chunksize);


    multinet->print_active_speech_commands(model_data);
    afe_handle->disable_wakenet(afe_data);
    afe_handle->disable_aec(afe_data);


    printf("------------detect start------------\n");
    while (task_flag) {
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); 
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE Fetch Fail");
            continue;
        }
        // multinet->clean(model_data);
        esp_mn_state_t mn_state = multinet->detect(model_data, res->data);
        if (mn_state == ESP_MN_STATE_DETECTING) {
            continue;
        }
        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = multinet->get_results(model_data);
            for (int i = 0; i < mn_result->num; i++) {
                printf("WAKE WORD %d, command_id: %d, phrase_id: %d, string: %s, prob: %f\n", 
                i+1, mn_result->command_id[i], mn_result->phrase_id[i], mn_result->string, mn_result->prob[i]);
                if (mn_result->command_id[i] == 1)
                {
                    printf("-----------Wake word detected-----------\n");
                    detect_flag = 1;
                }
            }
            // multinet->clean(model_data);
        }
        if (mn_state == ESP_MN_STATE_TIMEOUT) {
            esp_mn_results_t *mn_result = multinet->get_results(model_data);
            printf("Wake word string:%s\n", mn_result->string);
            multinet->clean(model_data);
            detect_flag = 0;
            continue;
        }
        if (detect_flag == 1)
        {
             printf("-----------listening-----------\n");
            res = afe_handle->fetch(afe_data); 
            if (!res || res->ret_value == ESP_FAIL) {
                printf("fetch error!\n");
                break;
            }
            
            esp_mn_state_t mn_state = multinet->detect(model_data, res->data);
            // if (mn_state == ESP_MN_STATE_DETECTING) {
            //     continue;
            // }
            while (mn_state == ESP_MN_STATE_DETECTING)
            {
                res = afe_handle->fetch(afe_data); 
                if (!res || res->ret_value == ESP_FAIL) {
                    printf("fetch error!\n");
                    break;
                }
                
                mn_state = multinet->detect(model_data, res->data);
            }
            
            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                for (int i = 0; i < mn_result->num; i++) {
                    printf("Command TOP %d, command_id: %d, phrase_id: %d, string: %s, prob: %f\n", 
                    i+1, mn_result->command_id[i], mn_result->phrase_id[i], mn_result->string, mn_result->prob[i]);
                }
                multinet->clean(model_data);
                detect_flag = 0;
               
            }
            if (mn_state == ESP_MN_STATE_TIMEOUT) {
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                printf("Command string:%s\n", mn_result->string);
                multinet->clean(model_data);
                detect_flag = 0;
                continue;
            }
        }
        
    }
    if (model_data) {
        multinet->destroy(model_data);
        model_data = NULL;
    }
    printf("detect exit\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    bsp_i2c_init();

    models = esp_srmodel_init("model"); // partition label defined in partitions.csv
    afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;

    afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);;
    afe_config.aec_init = false;
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);

    task_flag = 1;
    xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void*)afe_data, 5, NULL, 1);
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void*)afe_data, 5, NULL, 0);

    return ESP_OK;
err:
    app_sr_stop();
    return ret;

    //bsp_i2s_read((char *)audio_buffer, audio_chunksize * I2S_CHANNEL_NUM * sizeof(int16_t), &bytes_read, portMAX_DELAY);


}
