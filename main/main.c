#include "bsp/esp-bsp.h"
#include "device.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lv_demos.h"
#include "lv_examples.h"
#include "model_path.h"
#include "picotts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "PEBBLE"

#define TTS_CORE 1
#define MAX_STRING_LENGTH 60

static volatile bool detect_flag = false;
static volatile bool tts_running = false;
static esp_afe_sr_iface_t *afe_handle = NULL;

static srmodel_list_t *models = NULL;
static TaskHandle_t voice_handel = NULL;
static TaskHandle_t detect_handel = NULL;

/*Create an AZERTY keyboard map*/
static const char *kb_map[] = {"Q",
                               "W",
                               "E",
                               "R",
                               "T",
                               "Y",
                               "U",
                               "I",
                               "O",
                               "P",
                               LV_SYMBOL_BACKSPACE,
                               "\n",
                               "A",
                               "S",
                               "D",
                               "F",
                               "G",
                               "H",
                               "J",
                               "K",
                               "L",
                               LV_SYMBOL_NEW_LINE,
                               "\n",
                               "Z",
                               "X",
                               "C",
                               "V",
                               "B",
                               "N",
                               ",",
                               ".",
                               ":",
                               "!",
                               "?",
                               "\n",
                               LV_SYMBOL_CLOSE,
                               " ",
                               " ",
                               " ",
                               LV_SYMBOL_OK,
                               NULL};

/*Set the relative width of the buttons and other controls*/
static const lv_btnmatrix_ctrl_t kb_ctrl[] = {4, 4,
                                              4, 4,
                                              4, 4,
                                              4, 4,
                                              4, 4,
                                              6, 4,
                                              4, 4,
                                              4, 4,
                                              4, 4,
                                              4, 4,
                                              6, 4,
                                              4, 4,
                                              4, 4,
                                              4, 4,
                                              4, 4,
                                              4, 4,
                                              2, LV_BTNMATRIX_CTRL_HIDDEN | 2,
                                              6, LV_BTNMATRIX_CTRL_HIDDEN | 2,
                                              2};

typedef struct {
  char *token;
  char *(*fun)(int); // Function pointer member
} voice_mapping_t;

char *do_action(int id) {
  // char * res = malloc(MAX_STRING_LENGTH);
  switch (id) {
  case 0:
    return "Please repeat That";
    break;
  case 1:
    return "Hello Boss";
    break;
  case 2:
    return "Turning on";
    break;
  case 3: {
    return get_time(MAX_STRING_LENGTH - 1);
    break;
  }
  }

  return "repeat that";
}

#define MAX_COMMANDS 4

static const voice_mapping_t voice_lookup[MAX_COMMANDS] = {
    {"", do_action},          // default response
    {"Hi pebble", do_action}, // wake word
    {"Turn on the light", do_action},
    {"What day is it", do_action}};

static void on_samples(int16_t *buf, unsigned count) {
  esp_audio_play(buf, count * 2, 0);
}

static void on_tts_idel() { tts_running = false; }

static void wait_for_tts() {
  while (tts_running) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void say_this(char *received_message, size_t len) {
  vTaskSuspend(voice_handel);
  while (tts_running) {
    vTaskDelay(1);
  }
  picotts_add(received_message, len);
  tts_running = true;
  while (tts_running) {
    vTaskDelay(1);
  }
  vTaskResume(voice_handel);
}

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

  esp_mn_commands_clear(); // Clear commands that already exist
  for (size_t i = 1; i < MAX_COMMANDS; i++) {
    voice_mapping_t *voice = &voice_lookup[i];
    esp_mn_commands_add(i, voice->token);
  }
  esp_mn_commands_update(); // update commands
  int mu_chunksize = multinet->get_samp_chunksize(model_data);
  assert(mu_chunksize == afe_chunksize);
  multinet->print_active_speech_commands(model_data);

  unsigned prio = uxTaskPriorityGet(NULL);
  picotts_init(prio, on_samples, TTS_CORE);
  picotts_set_idle_notify(on_tts_idel);
  char message[MAX_STRING_LENGTH] = "";

  printf("------------detect start------------\n");

  while (true) {
    wait_for_tts();
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

        if (mn_result->command_id[i] != 1 && !detect_flag) {
          continue;
        }
        detect_flag = true;
        voice_mapping_t *voice = &voice_lookup[mn_result->command_id[i]];
        bzero(message, sizeof(message));
        strcpy(message, voice->fun(mn_result->command_id[i]));
        say_this(message, sizeof(message));
      }

      printf("-----------listening-----------\n");
    }

    if (mn_state == ESP_MN_STATE_TIMEOUT) {
      esp_mn_results_t *mn_result = multinet->get_results(model_data);
      printf("timeout, string:%s\n", mn_result->string);
      afe_handle->enable_wakenet(afe_data);
      afe_handle->disable_wakenet(afe_data);
      multinet->clean(model_data);
      detect_flag = false;
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

static void app_sr_init() {
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
                          &detect_handel, 0);
  xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void *)afe_data, 5,
                          &voice_handel, 1);
}

lv_obj_t *time_label;

void time_update_task(void *pvParameters) {
  while (true) {
    char *tim = "99:99 AM";
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(tim, 9, "%I:%M %p", &timeinfo);
    bsp_display_lock(0);
    lv_label_set_text(time_label, tim);
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void display_init() {
  bsp_display_lock(0);
  LV_IMG_DECLARE(rabbit);
  lv_obj_t *window = lv_tileview_create(lv_scr_act());
  lv_obj_t *window1 = lv_tileview_add_tile(window, 0, 0, LV_DIR_RIGHT);
  lv_obj_t *rabbit_gif = lv_gif_create(window1);
  lv_gif_set_src(rabbit_gif, &rabbit);
  lv_obj_align(rabbit_gif, LV_ALIGN_CENTER, 0, -20);
  time_label = lv_label_create(window1);
  lv_label_set_text(time_label, "99:99");
  lv_obj_set_style_text_font(time_label, &lv_font_montserrat_44, 0);
  // lv_obj_set_width(time_label, );  /*Set smaller width to make the lines
  // wrap*/
  lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -40);

  lv_obj_t *window2 = lv_tileview_add_tile(window, 1, 0, LV_DIR_LEFT);

  lv_obj_t *kb = lv_keyboard_create(window2);

  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_map, kb_ctrl);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);

  /*Create a text area. The keyboard will write here*/
  lv_obj_t *ta;
  ta = lv_textarea_create(window2);
  lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_size(ta, lv_pct(90), 80);
  lv_obj_add_state(ta, LV_STATE_FOCUSED);

  lv_keyboard_set_textarea(kb, ta);
  bsp_display_unlock();

  xTaskCreate(&time_update_task, "timer task", 2 * 1024, NULL, 3, NULL);
}

void app_main(void) {
  device_init();
  bsp_spiffs_mount();
  bsp_i2c_init();
  bsp_display_start();
  bsp_display_backlight_on();
  bsp_display_brightness_set(10);
  app_sr_init();
  display_init();
}
