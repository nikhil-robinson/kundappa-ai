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
#include "esp_mn_speech_commands.h"
#include "esp_board_init.h"

#include "esp_log.h"

#define TAG "KUNDAPPA"
#define I2S_CHANNEL_NUM      2

int detect_flag = 0;
static esp_afe_sr_iface_t *afe_handle = NULL;
srmodel_list_t *models = NULL;


void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int nch = afe_handle->get_channel_num(afe_data);
    int feed_channel = esp_get_feed_channel();
    assert(nch <= feed_channel);
    int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
    assert(i2s_buff);

    while (true) {
        esp_get_feed_data(false, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);

        afe_handle->feed(afe_data, i2s_buff);
    }
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
    esp_mn_commands_add(1, "Hi pebble");   // add a command
    esp_mn_commands_add(2, "Turn on the light");  // add a command
    esp_mn_commands_update();                      // update commands


    int mu_chunksize = multinet->get_samp_chunksize(model_data);
    assert(mu_chunksize == afe_chunksize);


    multinet->print_active_speech_commands(model_data);
    afe_handle->disable_wakenet(afe_data);
    afe_handle->disable_aec(afe_data);


    printf("------------detect start------------\n");
    while (true) {
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
    models = esp_srmodel_init("model"); // partition label defined in partitions.csv
    ESP_ERROR_CHECK(esp_board_init(16000, 1, 16));
    afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;
    afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);;
    afe_config.aec_init = false;
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void*)afe_data, 5, NULL, 1);
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void*)afe_data, 5, NULL, 0);

}
