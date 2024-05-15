#include "esp_attr.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "iot_button.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "stdio.h"
#include <device.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <sdkconfig.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "DEVICE TEST";

#define TEST_MEMORY_LEAK_THRESHOLD (-400)
#define BUTTON_IO_NUM 0
#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_NUM 16

static button_handle_t g_btns[BUTTON_NUM] = {0};

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 48
#endif

#define EXAMPLE_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static void obtain_time(void);
void set_timestamp(char *time_stamp);

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about
 * two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    char *timestamp = (char *)malloc(64);
    set_timestamp(timestamp);
    ESP_LOGI(TAG, "%s", timestamp);
    free(timestamp);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void wifi_init_sta(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = EXAMPLE_ESP_WIFI_SSID,
              .password = EXAMPLE_ESP_WIFI_PASS,
              /* Authmode threshold resets to WPA2 as default if password
               * matches WPA2 standards (password len => 8). If you want to
               * connect the device to deprecated WEP/WPA networks, Please set
               * the threshold value to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set
               * the password with length and format matching to
               * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
               */
              .threshold =
                  {
                      .authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
                  },
              .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
              .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
          },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or
   * connection failed for the maximum number of re-tries (WIFI_FAIL_BIT). The
   * bits are set by event_handler() (see above) */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we
   * can test which event actually happened. */
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", EXAMPLE_ESP_WIFI_SSID,
             EXAMPLE_ESP_WIFI_PASS);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }
}

void time_sync_notification_cb(struct timeval *tv) {
  ESP_LOGI(TAG, "Notification of a time synchronization event");
}

void set_timestamp(char *time_stamp) {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  // Is time set? If not, tm_year will be (1970 - 1900).
  if (timeinfo.tm_year < (2016 - 1900)) {
    ESP_LOGI(
        TAG,
        "Time is not set yet. Connecting to WiFi and getting time over NTP.");
    obtain_time();
    // update 'now' variable with current time
    time(&now);
  }

  char strftime_buf[64];

  setenv("TZ", "IST-05:30:00", 1);
  tzset();
  localtime_r(&now, &timeinfo);
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
  ESP_LOGI(TAG, "The current date/time in India is: %s", strftime_buf);

  if (sntp_get_sync_mode() == SNTP_SYNC_MODE_SMOOTH) {
    struct timeval outdelta;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS) {
      adjtime(NULL, &outdelta);
      ESP_LOGI(
          TAG,
          "Waiting for adjusting time ... outdelta = %jd sec: %li ms: %li us",
          (intmax_t)outdelta.tv_sec, outdelta.tv_usec / 1000,
          outdelta.tv_usec % 1000);
      vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
  }
  memcpy(time_stamp, strftime_buf, sizeof(strftime_buf));
}

void go_to_sleep(int deep_sleep_sec) {
  ESP_LOGI(TAG, "Entering deep sleep for %d seconds", deep_sleep_sec);
  esp_deep_sleep(1000000LL * deep_sleep_sec);
}

static void obtain_time(void) {
  esp_sntp_config_t config =
      ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SNTP_TIME_SERVER);
  config.sync_cb =
      time_sync_notification_cb; // Note: This is only needed if we want

  esp_netif_sntp_init(&config);

  // wait for time to be set
  time_t now = 0;
  struct tm timeinfo = {0};
  int retry = 0;
  const int retry_count = 15;
  while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) ==
             ESP_ERR_TIMEOUT &&
         ++retry < retry_count) {
    ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry,
             retry_count);
  }
  time(&now);
  localtime_r(&now, &timeinfo);
  esp_netif_sntp_deinit();
}


unsigned long get_epoch_time() {
  time_t now;
  time(&now);
  return now;
}

static int get_btn_index(button_handle_t btn) {
  for (size_t i = 0; i < BUTTON_NUM; i++) {
    if (btn == g_btns[i]) {
      return i;
    }
  }
  return -1;
}

static void button_press_down_cb(void *arg, void *data) {
  if (iot_button_get_event(arg) == BUTTON_PRESS_DOWN)
    ESP_LOGI(TAG, "BTN%d: BUTTON_PRESS_DOWN",
             get_btn_index((button_handle_t)arg));
}

static void button_press_up_cb(void *arg, void *data) {
  if (iot_button_get_event(arg) == BUTTON_PRESS_UP)
    ESP_LOGI(TAG, "BTN%d: BUTTON_PRESS_UP[%d]",
             get_btn_index((button_handle_t)arg),
             iot_button_get_ticks_time((button_handle_t)arg));
}

static void button_press_repeat_cb(void *arg, void *data) {
  ESP_LOGI(TAG, "BTN%d: BUTTON_PRESS_REPEAT[%d]",
           get_btn_index((button_handle_t)arg),
           iot_button_get_repeat((button_handle_t)arg));
}

static void button_single_click_cb(void *arg, void *data) {
  if (iot_button_get_event(arg) == BUTTON_SINGLE_CLICK)
    ESP_LOGI(TAG, "BTN%d: BUTTON_SINGLE_CLICK",
             get_btn_index((button_handle_t)arg));
}

static void button_double_click_cb(void *arg, void *data) {
  if (iot_button_get_event(arg) == BUTTON_DOUBLE_CLICK)
    ESP_LOGI(TAG, "BTN%d: BUTTON_DOUBLE_CLICK",
             get_btn_index((button_handle_t)arg));
}

static void button_long_press_start_cb(void *arg, void *data) {
  if (iot_button_get_event(arg) == BUTTON_LONG_PRESS_START)
    ESP_LOGI(TAG, "BTN%d: BUTTON_LONG_PRESS_START",
             get_btn_index((button_handle_t)arg));
}

static void button_long_press_hold_cb(void *arg, void *data) {
  if (iot_button_get_event(arg) == BUTTON_LONG_PRESS_HOLD)
    ESP_LOGI(TAG, "BTN%d: BUTTON_LONG_PRESS_HOLD[%d],count is [%d]",
             get_btn_index((button_handle_t)arg),
             iot_button_get_ticks_time((button_handle_t)arg),
             iot_button_get_long_press_hold_cnt((button_handle_t)arg));
}

static void button_press_repeat_done_cb(void *arg, void *data) {
  if (iot_button_get_event(arg) == BUTTON_PRESS_REPEAT_DONE)
    ESP_LOGI(TAG, "BTN%d: BUTTON_PRESS_REPEAT_DONE[%d]",
             get_btn_index((button_handle_t)arg),
             iot_button_get_repeat((button_handle_t)arg));
}

void configure_button() {
  button_config_t cfg = {
      .type = BUTTON_TYPE_GPIO,
      .long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS,
      .short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS,
      .gpio_button_config =
          {
              .gpio_num = CONFIG_CONTROL_BUTTON_GPIO,
              .active_level = 0,
          },
  };
  g_btns[0] = iot_button_create(&cfg);

  iot_button_register_cb(g_btns[0], BUTTON_PRESS_DOWN, button_press_down_cb,
                         NULL);
  iot_button_register_cb(g_btns[0], BUTTON_PRESS_UP, button_press_up_cb, NULL);
  iot_button_register_cb(g_btns[0], BUTTON_PRESS_REPEAT, button_press_repeat_cb,
                         NULL);
  iot_button_register_cb(g_btns[0], BUTTON_SINGLE_CLICK, button_single_click_cb,
                         NULL);
  iot_button_register_cb(g_btns[0], BUTTON_DOUBLE_CLICK, button_double_click_cb,
                         NULL);
  iot_button_register_cb(g_btns[0], BUTTON_LONG_PRESS_START,
                         button_long_press_start_cb, NULL);
  iot_button_register_cb(g_btns[0], BUTTON_LONG_PRESS_HOLD,
                         button_long_press_hold_cb, NULL);
  iot_button_register_cb(g_btns[0], BUTTON_PRESS_REPEAT_DONE,
                         button_press_repeat_done_cb, NULL);
}

void device_init() {
  configure_button();
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
  wifi_init_sta();
}

void device_deinit() { iot_button_delete(g_btns[0]); }