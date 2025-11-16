#include <stdio.h>
#include <string.h> // For string functions

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" // For our "signals" (semaphores)
#include "esp_log.h"
#include "nvs_flash.h" // For Wi-Fi storage

#include "esp_wifi.h"    // For Wi-Fi
#include "esp_event.h"   // For event handling
#include "esp_netif.h"   // For TCP/IP stack
#include "mqtt_client.h" // For MQTT
#include "driver/gpio.h" // For GPIO pins
#include "esp_sleep.h"   // For Deep Sleep
#include "math.h"

// Your credentials file
#include "credentials.h"

// ================ CONFIG ==================
#define D0_PIN GPIO_NUM_4    // Door sensor pin
#define LED_PIN GPIO_NUM_2   // Onboard LED
static const char *TAG = "INTRUSION_SYS";

// --- Semaphores to signal when connections are complete ---
static SemaphoreHandle_t s_wifi_sem;
static SemaphoreHandle_t s_mqtt_sem;

// --- State flags ---
static bool s_wifi_connected = false;
static bool s_mqtt_connected = false;

// ==========================================================
//               EVENT HANDLERS
// ==========================================================

/*
 * Event handler for Wi-Fi and IP events.
 * Its only job is to signal the main task.
 */
static void wifi_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected.");
        s_wifi_connected = false;
        xSemaphoreGive(s_wifi_sem); // Signal that the connection attempt is over
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP address.");
        s_wifi_connected = true;
        xSemaphoreGive(s_wifi_sem); // Signal that the connection attempt is over (SUCCESS)
    }
}

/*
 * Event handler for MQTT events.
 * This version has the CRITICAL POINTER FIX.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    // --- THIS IS THE FIX ---
    // event_data is a pointer to the event struct.
    // The client handle is *inside* that struct.
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    // --- END OF FIX ---

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to ThingsBoard!");
        s_mqtt_connected = true;

        // Subscribe to RPC (optional, but good)
        esp_mqtt_client_subscribe(client, "v1/devices/me/rpc/request/+", 1);

        // Signal the main task that we are connected
        xSemaphoreGive(s_mqtt_sem);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from ThingsBoard.");
        s_mqtt_connected = false;

        // Signal the main task that the connection is over
        xSemaphoreGive(s_mqtt_sem);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT Message received.");
        break;

    default:
        break;
    }
}

// ==========================================================
//               THE SYNCHRONOUS ALERT TASK
// ==========================================================

/*
 * This is our main synchronous "task".
 * It initializes Wi-Fi, connects, sends an MQTT alert, and then
 * completely de-initializes all network systems.
 */
esp_err_t send_alert_sync(void)
{
    // Create semaphores
    s_wifi_sem = xSemaphoreCreateBinary();
    s_mqtt_sem = xSemaphoreCreateBinary();
    s_wifi_connected = false;
    s_mqtt_connected = false;

    // --- 1. Initialize Network Stack ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register our clean, separate Wi-Fi/IP handler
    esp_event_handler_instance_t instance_wifi;
    esp_event_handler_instance_t instance_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_ip_event_handler, NULL, &instance_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_ip_event_handler, NULL, &instance_ip));

    // --- 2. Connect to Wi-Fi ---
    wifi_config_t wifi_config = {.sta = {.ssid = SSID, .password = PWD}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    // Wait for Wi-Fi connection (15-sec timeout). This BLOCKS the code.
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    if (xSemaphoreTake(s_wifi_sem, pdMS_TO_TICKS(15000)) == pdFALSE || !s_wifi_connected) {
        ESP_LOGE(TAG, "Wi-Fi connection failed or timed out.");
        goto cleanup_wifi; // Jump to cleanup
    }

    // --- 3. Connect to MQTT ---
    ESP_LOGI(TAG, "Wi-Fi connected. Starting MQTT.");
    esp_mqtt_client_config_t mqtt_cfg = {.broker.address.uri = BROKER_URL, .credentials.username = ACCESS_TOKEN};
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);

    // Register your clean, separate MQTT handler
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    // Wait for MQTT connection (10-sec timeout). This BLOCKS the code.
    ESP_LOGI(TAG, "Waiting for MQTT connection...");
    if (xSemaphoreTake(s_mqtt_sem, pdMS_TO_TICKS(10000)) == pdFALSE || !s_mqtt_connected) {
        ESP_LOGE(TAG, "MQTT connection failed or timed out.");
        goto cleanup_mqtt; // Jump to MQTT cleanup
    }

    // --- 4. SUCCESS: Publish Alert ---
    ESP_LOGI(TAG, "MQTT connected! Publishing alert...");
    const char *alert_msg = "{\"intrusion\": true}";
    esp_mqtt_client_publish(client, "v1/devices/me/telemetry", alert_msg, 0, 1, 0);
    ESP_LOGW(TAG, "🚨 Intrusion alert sent to ThingsBoard!");
    
    // Give 2 seconds for publish to send before disconnect
    vTaskDelay(pdMS_TO_TICKS(2000)); 

// --- 5. Full Cleanup (to prevent crash on next boot) ---
    cleanup_mqtt:
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);

    cleanup_wifi:
        esp_wifi_stop();
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_ip);
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_wifi);
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(sta_netif);
        esp_event_loop_delete_default();

    vSemaphoreDelete(s_wifi_sem);
    vSemaphoreDelete(s_mqtt_sem);

    return (s_mqtt_connected) ? ESP_OK : ESP_FAIL;
}

// ==========================================================
//               APP MAIN (CLEAN AND SIMPLE)
// ==========================================================

void app_main(void)
{
    // ----- Init NVS (required on every boot) -----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ----- Check Wakeup Reason -----
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGW(TAG, "🚨 Intrusion Detected! Waking up...");

        // Blink LED to indicate activity
        gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(LED_PIN, 1);

        // --- THIS IS OUR SYNCHRONOUS TASK ---
        // This function will block until it succeeds or times out.
        // It handles all init, connect, send, and cleanup.
        send_alert_sync();

        // Turn LED off when done
        gpio_set_level(LED_PIN, 0);

    } else {
        ESP_LOGI(TAG, "System booting normally.");
    }

    // ----- Configure wakeup & Sleep -----
    ESP_LOGI(TAG, "Configuring EXT0 wake-up on GPIO %d for HIGH level (door open)", D0_PIN);
    esp_sleep_enable_ext0_wakeup(D0_PIN, 1); // Wake up on HIGH

    ESP_LOGI(TAG, "Going to deep sleep...");
    esp_log_level_set("*", ESP_LOG_NONE); // Silence logs during sleep
    esp_deep_sleep_start();
}