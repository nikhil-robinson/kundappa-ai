#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_board_init.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "picotts.h"
#include <string.h>

#define TTS_CORE 1

#define TAG "KUNDAPPA"
#define I2S_CHANNEL_NUM 2

#define QUEUE_LENGTH 5
#define MAX_STRING_LENGTH 60

static volatile bool detect_flag = false;
static volatile bool tts_running = false;
static esp_afe_sr_iface_t *afe_handle = NULL;
srmodel_list_t *models = NULL;
QueueHandle_t xQueue = NULL;

const char greeting[] = "This is a test";

TaskHandle_t voice_handel = NULL;
TaskHandle_t detect_handel = NULL;

static void on_samples(int16_t *buf, unsigned count) {
  esp_audio_play(buf, count * 2, 0);
}

static void on_tts_idel() { tts_running = false; }
void feed_Task(void *arg) {
  esp_afe_sr_data_t *afe_data = arg;
  int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
  int nch = afe_handle->get_channel_num(afe_data);
  int feed_channel = esp_get_feed_channel();
  assert(nch <= feed_channel);
  int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
  assert(i2s_buff);

  while (true) {
    esp_get_feed_data(false, i2s_buff,
                      audio_chunksize * sizeof(int16_t) * feed_channel);

    afe_handle->feed(afe_data, i2s_buff);
  }
}

void detect_Task(void *arg) {
  esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;
  int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
  printf("multinet:%s\n", mn_name);
  esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
  model_iface_data_t *model_data = multinet->create(mn_name, 6000);
  esp_mn_commands_clear();             // Clear commands that already exist
  esp_mn_commands_add(1, "Hi pebble"); // add a command
  esp_mn_commands_add(2, "Turn on the light"); // add a command
  esp_mn_commands_update();                    // update commands
  int mu_chunksize = multinet->get_samp_chunksize(model_data);
  assert(mu_chunksize == afe_chunksize);
  multinet->print_active_speech_commands(model_data);

  char message[MAX_STRING_LENGTH];

  printf("------------detect start------------\n");

  while (true) {
    afe_fetch_result_t *res = afe_handle->fetch(afe_data);
    if (!res || res->ret_value == ESP_FAIL) {
      printf("fetch error!\n");
      break;
    }

    esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

    if (mn_state == ESP_MN_STATE_DETECTING) {
      continue;
    }

    if (mn_state == ESP_MN_STATE_DETECTED) {
      esp_mn_results_t *mn_result = multinet->get_results(model_data);
      for (int i = 0; i < mn_result->num; i++) {
        printf("TOP %d, command_id: %d, phrase_id: %d, string: %s, prob: %f\n",
               i + 1, mn_result->command_id[i], mn_result->phrase_id[i],
               mn_result->string, mn_result->prob[i]);
        if (detect_flag) {
          switch (mn_result->command_id[i]) {
          case 2: {
            strcpy(message, "Hello, Nikhil!");
            xQueueSend(xQueue, message, portMAX_DELAY);
            break;
          }
          case 3: {
            strcpy(message, "Hello, Nikhil!");
            xQueueSend(xQueue, message, portMAX_DELAY);
            break;
          }
          default: {
            break;
          }
          }
        }
        if (mn_result->command_id[i] == 1) {
          strcpy(message, "Hello, Nikhil!");
          xQueueSend(xQueue, message, portMAX_DELAY);
          detect_flag = false;
        }
      }

      printf("-----------listening-----------\n");
    }

    if (mn_state == ESP_MN_STATE_TIMEOUT) {
      esp_mn_results_t *mn_result = multinet->get_results(model_data);
      printf("timeout, string:%s\n", mn_result->string);
      afe_handle->enable_wakenet(afe_data);
      afe_handle->disable_wakenet(afe_data);
      multinet->clean(model_data);
      detect_flag = 0;
      printf("\n-----------awaits to be waken up-----------\n");
      continue;
    }
  }
  if (model_data) {
    multinet->destroy(model_data);
    model_data = NULL;
  }
  printf("detect exit\n");
  vTaskDelete(NULL);
}

void audio_paly_back(void *arg) {
  unsigned prio = uxTaskPriorityGet(NULL);
  picotts_init(prio, on_samples, TTS_CORE);
  picotts_set_idle_notify(on_tts_idel);
  char received_message[MAX_STRING_LENGTH];
  while (true) {
    if (xQueueReceive(xQueue, received_message, portMAX_DELAY) == pdTRUE) {
      printf("Received message: %s\n", received_message);
      vTaskSuspend(voice_handel);
      while (tts_running) {
        vTaskDelay(1);
      }
      picotts_add(received_message, sizeof(received_message));
      tts_running = true;
      while (tts_running) {
        vTaskDelay(1);
      }
      vTaskResume(voice_handel);
    }
  }
  picotts_shutdown();
}

void app_main(void) {
  xQueue = xQueueCreate(QUEUE_LENGTH, MAX_STRING_LENGTH);
  if (xQueue == NULL) {
    printf("Failed to create queue\n");
    return;
  }
  models =
      esp_srmodel_init("model"); // partition label defined in partitions.csv
  ESP_ERROR_CHECK(esp_board_init(16000, 1, 16));
  esp_audio_set_play_vol(100);
  afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;
  afe_config_t afe_config = AFE_CONFIG_DEFAULT();
  afe_config.wakenet_model_name =
      esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
  afe_config.aec_init = false;
  esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);
  xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void *)afe_data, 5,
                          &detect_handel, 1);
  xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void *)afe_data, 5,
                          &voice_handel, 0);
  xTaskCreatePinnedToCore(&audio_paly_back, "play", 8 * 1024, NULL, 5, NULL, 0);
}
