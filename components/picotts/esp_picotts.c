/* Copyright (C) 2024 DiUS Computing Pty Ltd.
 * Licensed under the Apache 2.0 license.
 *
 * @author J Mattsson <jmattsson@dius.com.au>
 */
#include "picotts.h"
#include "picoapi.h"
#include "picoapid.h"
#include "esp_picorsrc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

// Yep, that's 1.1MB needed by PicoTTS, not counting the resource files which
// we access directly from flash.
#define PICO_MEM_SIZE 1100000

#define PICOTASK_EXIT  0x0000001u

#define IDLE_WAIT_COUNT 5

static picotts_output_fn outputCb;
static picotts_error_notify_fn errorCb;
static picotts_idle_notify_fn idleCb;

static SemaphoreHandle_t exitLock;
static QueueHandle_t textQ;
static TaskHandle_t picoTask;

static void *picoMemArea;

static pico_System   picoSystem;
static pico_Resource picoTaResource;
static pico_Resource picoSgResource;
static pico_Engine   picoEngine;

static const pico_Char voiceName[] = "PicoVoice";
static const char tag[] = "picotts";


#ifdef CONFIG_PICOTTS_RESOURCE_MODE_EMBED
// Embedded resources
extern const char ta_bin_start[] asm("_binary_picotts_ta_bin_start");
extern const char sg_bin_start[] asm("_binary_picotts_sg_bin_start");

#else

static esp_partition_mmap_handle_t taMmap;
static esp_partition_mmap_handle_t sgMmap;

static const void *find_and_map_partition(
    const char *name, esp_partition_mmap_handle_t *handle)
{
  const esp_partition_t *part =
    esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, name);
  if (!part)
  {
    ESP_LOGE(tag, "Partition '%s' not found", name);
    return NULL;
  }
  else
  {
    const void *ptr = 0;
    esp_err_t ret = esp_partition_mmap(
      part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, handle);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ret);
    ESP_LOGI(tag, "Partition '%s' mmap'd to %p", name, ptr);
    return (ret == ESP_OK) ? ptr : NULL;
  }
}

static void unmap_partitions(void)
{
  if (taMmap)
  {
    esp_partition_munmap(taMmap);
    taMmap = 0;
  }
  if (sgMmap)
  {
    esp_partition_munmap(sgMmap);
    sgMmap = 0;
  }
}
#endif


static const void *find_ta_bin_start()
{
#ifdef CONFIG_PICOTTS_RESOURCE_MODE_EMBED
  return ta_bin_start;
#else
  return find_and_map_partition(CONFIG_PICOTTS_TA_PARTITION, &taMmap);
#endif
}


static const void *find_sg_bin_start()
{
#ifdef CONFIG_PICOTTS_RESOURCE_MODE_EMBED
  return sg_bin_start;
#else
  return find_and_map_partition(CONFIG_PICOTTS_SG_PARTITION, &sgMmap);
#endif
}


// Use regular exp() function as the picotty hack doesn't work on Xtensa
picoos_double picoos_quick_exp(const picoos_double y)
{
  return exp(y);
}


static void esp_pico_err_print(const char *what, int code)
{
  pico_Retstring msg;
  pico_getSystemStatusMessage(picoSystem, code, msg);
  ESP_LOGE(tag, "%s (%i): %s", what, code, msg);
}


static void esp_pico_run(void *)
{
  ESP_LOGI(tag, "Task started");
  bool error = false;
  enum {
    WAITING_FOR_BYTES, WAITING_FOR_OUTPUT
  } state = WAITING_FOR_BYTES;
  unsigned idles = 0;

  while(!error)
  {
    uint32_t flags;
    if (xTaskNotifyWait(0, ~0, &flags, 0) == pdPASS)
    {
      if (flags & PICOTASK_EXIT)
        break;
    }

    uint8_t c;
    while (xQueuePeek(textQ, &c, 0) == pdPASS)
    {
      int16_t processed = 0;
      int ret = pico_putTextUtf8(picoEngine, &c, 1, &processed);
      if (ret)
      {
        esp_pico_err_print("Put text failed, stopping TTS", ret);
        error = true;
        break;
      }
      else
      {
        if (processed)
        {
          xQueueReceive(textQ, &c, 0);
          if (state == WAITING_FOR_BYTES)
            state = WAITING_FOR_OUTPUT;
        }
      }
    }

    switch (state)
    {
      case WAITING_FOR_BYTES:
        if (idles < IDLE_WAIT_COUNT)
        {
          if (++idles == IDLE_WAIT_COUNT && idleCb)
            idleCb();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
      case WAITING_FOR_OUTPUT:
      {
        int status;
        do {
          int16_t outbuf[128];
          int16_t bytes = 0, type = 0;
          // Note: Only PICO_DATA_PCM_16BIT is defined as output type, so we
          // don't propagate that information. Rather, it's a fixed property.
          status =
            pico_getData(picoEngine, outbuf, sizeof(outbuf), &bytes, &type);
          if (bytes > 0)
            outputCb(outbuf, bytes/2);
        } while (status == PICO_STEP_BUSY);
        if (status != PICO_STEP_IDLE)
        {
          esp_pico_err_print("Get data failed, stopping TTS", status);
          error = true;
        }
        else
        {
          state = WAITING_FOR_BYTES;
          idles = 0;
        }
        break;
       }
    }
  }
  ESP_LOGI(tag, "Exiting task");
  xSemaphoreGive(exitLock);
  vTaskDelete(NULL);

  if (error && errorCb)
    errorCb();
}


static void esp_pico_cleanup(void)
{
  if (picoTask)
  {
    xTaskNotify(picoTask, PICOTASK_EXIT, eSetBits);
    xSemaphoreTake(exitLock, portMAX_DELAY);
    picoTask = NULL;
  }

  if (picoEngine)
  {
    pico_disposeEngine(picoSystem, &picoEngine);
    pico_releaseVoiceDefinition(picoSystem, voiceName);
    picoEngine = NULL;
  }

  if (picoSgResource)
  {
    esp_pico_unloadResource(picoSystem, &picoSgResource);
    picoSgResource = NULL;
  }

  if (picoTaResource)
  {
    esp_pico_unloadResource(picoSystem, &picoTaResource);
    picoTaResource = NULL;
  }

  if (picoSystem)
  {
    pico_terminate(&picoSystem);
    picoSystem = NULL;
  }

  free(picoMemArea);
  picoMemArea = NULL;

  if (textQ)
  {
    vQueueDelete(textQ);
    textQ = NULL;
  }
}


bool picotts_init(unsigned prio, picotts_output_fn cb, int core)
{
  if (!exitLock)
    exitLock = xSemaphoreCreateBinary();

  outputCb = cb;

  if (picoMemArea)
  {
    ESP_LOGE(tag, "already initialized");
    return false;
  }
  picoMemArea = malloc(PICO_MEM_SIZE);
  if (!picoMemArea)
  {
    ESP_LOGE(tag, "insufficient memory to initialize picotts");
    return false;
  }

  #define PICO_INIT_CHECK(msg) \
    if (ret != 0) \
    { \
      esp_pico_err_print(msg, ret); \
      esp_pico_cleanup(); \
      return false; \
    }

  int ret = pico_initialize(picoMemArea, PICO_MEM_SIZE, &picoSystem);
  PICO_INIT_CHECK("init failed");

  const void *ta = find_ta_bin_start();
  if (!ta)
  {
    ESP_LOGE(tag, "Unable to find text analysis resource");
    return false;
  }
  else
    ESP_LOGI(tag, "Loading text analysis resource from %p", ta);

  ret = esp_pico_loadResource(picoSystem, ta, &picoTaResource);
  PICO_INIT_CHECK("Text analysis load failed");

  const void *sg = find_sg_bin_start();
  if (!sg)
  {
    ESP_LOGE(tag, "Unable to find signal generator resource");
    return false;
  }
  else
    ESP_LOGI(tag, "Loading signal generator resource from %p", sg);

  ret = esp_pico_loadResource(picoSystem, sg, &picoSgResource);
  PICO_INIT_CHECK("Signal generator load failed");

  ret = pico_createVoiceDefinition(picoSystem, voiceName);
  PICO_INIT_CHECK("Voice creation failed");

  pico_Retstring str;

  ret = pico_getResourceName(picoSystem, picoTaResource, str);
  PICO_INIT_CHECK("TA resource name error");
  ret = pico_addResourceToVoiceDefinition(
    picoSystem, voiceName, (const pico_Char *)str);
  PICO_INIT_CHECK("TA resource add failed");

  ret = pico_getResourceName(picoSystem, picoSgResource, str);
  PICO_INIT_CHECK("SG resource name error");
  ret = pico_addResourceToVoiceDefinition(
    picoSystem, voiceName, (const pico_Char *)str);
  PICO_INIT_CHECK("SG resource add failed");

  ret = pico_newEngine(picoSystem, voiceName, &picoEngine);
  PICO_INIT_CHECK("Engine creation failed");

  #undef PICO_INIT_CHECK

  textQ = xQueueCreate(CONFIG_PICOTTS_INPUT_QUEUE_SIZE, sizeof(char));
  if (!textQ)
  {
    ESP_LOGE(tag, "Failed to create input queue");
    esp_pico_cleanup();
    return false;
  }

  if (xTaskCreatePinnedToCore(esp_pico_run, "picotts", 8192, NULL,
        prio, &picoTask, core == -1 ? tskNO_AFFINITY : core)
      != pdPASS)
  {
    ESP_LOGE(tag, "Failed to create task");
    esp_pico_cleanup();
    return false;
  }

  return true;
}


void picotts_add(const char *text, unsigned len)
{
  while(len--)
    xQueueSendToBack(textQ, text++, portMAX_DELAY);
}


void picotts_shutdown(void)
{
  esp_pico_cleanup();

#if CONFIG_PICOTTS_RESOURCE_MODE_PARTITION
  unmap_partitions();
#endif
}


void picotts_set_error_notify(picotts_error_notify_fn cb)
{
  errorCb = cb;
}


void picotts_set_idle_notify(picotts_idle_notify_fn cb)
{
  idleCb = cb;
}
