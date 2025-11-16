#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "esp_sleep.h"
#include "driver/gpio.h"
#include "math.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sys.h"

// Main Parameter
#define D0 GPIO_NUM_4

char *TAG = "INTRUSION ALERT";

void app_main(void)
{
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      ESP_LOGW(TAG, "ALERT! Intusion Detected");
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
      ESP_LOGI(TAG, "System is booting up for the first time.");
      break;
  }

  ESP_LOGI(TAG, "Configuring wake-up for HIGH (door open) on GPIO PIN");
  esp_sleep_enable_ext0_wakeup(D0, 1); // 1 = Wake on HIGH
 

  ESP_LOGI(TAG, "Going to deep sleep now.");
  esp_log_level_set("*", ESP_LOG_NONE);
  esp_deep_sleep_start();
}
