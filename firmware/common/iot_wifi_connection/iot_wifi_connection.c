#include "iot_wifi_connection.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "iot_wifi_conn";

static bool s_initialized;
static volatile bool s_connected;
static bool s_ever_connected;
static volatile uint32_t s_retry_count;
static uint32_t s_retry_base_ms;
static uint32_t s_retry_max_ms;
static void *s_callback_arg;
static iot_wifi_connection_callbacks_t s_callbacks;
static EventGroupHandle_t s_event_group;
static TaskHandle_t s_task;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

static bool callback_action_allowed(iot_wifi_connection_action_t action)
{
    if (s_callbacks.action_allowed == NULL) {
        return true;
    }
    return s_callbacks.action_allowed(action, s_callback_arg);
}

static void callback_on_retry(uint32_t retry_count, uint32_t delay_ms)
{
    if (s_callbacks.on_retry != NULL) {
        s_callbacks.on_retry(retry_count, delay_ms, s_callback_arg);
    }
}

static void callback_on_disconnected(int reason, int rssi)
{
    if (s_callbacks.on_disconnected != NULL) {
        s_callbacks.on_disconnected(reason, rssi, s_callback_arg);
    }
}

static void callback_on_got_ip(uint32_t ip_address)
{
    if (s_callbacks.on_got_ip != NULL) {
        s_callbacks.on_got_ip(ip_address, s_callback_arg);
    }
}

static void callback_on_first_connected(void)
{
    if (s_callbacks.on_first_connected != NULL) {
        s_callbacks.on_first_connected(s_callback_arg);
    }
}

static void connection_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (!s_connected) {
            if (!callback_action_allowed(
                    IOT_WIFI_CONNECTION_ACTION_RECONNECT_LOOP)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            const uint32_t retry_count = s_retry_count;
            const uint32_t shift = retry_count < 6U ? retry_count : 6U;
            uint64_t delay_ms = (uint64_t)s_retry_base_ms * (1ULL << shift);
            if (delay_ms > s_retry_max_ms) {
                delay_ms = s_retry_max_ms;
            }

            callback_on_retry(retry_count, (uint32_t)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            if (s_connected) {
                break;
            }

            if (!callback_action_allowed(
                    IOT_WIFI_CONNECTION_ACTION_MANUAL_DISCONNECT)) {
                continue;
            }
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));

            if (!callback_action_allowed(
                    IOT_WIFI_CONNECTION_ACTION_MANUAL_CONNECT)) {
                continue;
            }
            const esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi connect returned: %s",
                         esp_err_to_name(err));
            }
            ++s_retry_count;

            if ((s_retry_count % 5U) == 0U &&
                callback_action_allowed(
                    IOT_WIFI_CONNECTION_ACTION_STOP_BEFORE_RECOVERY)) {
                ESP_LOGW(TAG, "Wi-Fi recovery: stop/start");
                esp_wifi_stop();
                vTaskDelay(pdMS_TO_TICKS(500));
                if (callback_action_allowed(
                        IOT_WIFI_CONNECTION_ACTION_START_AFTER_STOP)) {
                    esp_wifi_start();
                }
            }
        }
    }
}

static void connection_event_cb(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (callback_action_allowed(
                IOT_WIFI_CONNECTION_ACTION_STA_START_CONNECT)) {
            esp_wifi_connect();
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        callback_on_disconnected(event->reason, event->rssi);

        if (callback_action_allowed(
                IOT_WIFI_CONNECTION_ACTION_DISCONNECTED_HANDOFF)) {
            s_connected = false;
            (void)xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
            xTaskNotifyGive(s_task);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (!callback_action_allowed(IOT_WIFI_CONNECTION_ACTION_GOT_IP)) {
            return;
        }

        s_retry_count = 0U;
        s_connected = true;
        if (!s_ever_connected) {
            s_ever_connected = true;
            callback_on_first_connected();
        }
        callback_on_got_ip(event->ip_info.ip.addr);
        (void)xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t iot_wifi_connection_initialize(
    const iot_wifi_connection_config_t *config)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL || config->callbacks == NULL ||
        config->callbacks->action_allowed == NULL ||
        config->callbacks->on_retry == NULL ||
        config->callbacks->on_got_ip == NULL ||
        config->retry_base_ms == 0U || config->retry_max_ms == 0U ||
        config->retry_max_ms < config->retry_base_ms ||
        config->task_stack_size == 0U ||
        config->task_priority >= configMAX_PRIORITIES) {
        return ESP_ERR_INVALID_ARG;
    }

    s_callbacks = *config->callbacks;
    s_callback_arg = config->callback_arg;
    s_retry_base_ms = config->retry_base_ms;
    s_retry_max_ms = config->retry_max_ms;

    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreate(connection_task, "wifi-conn",
                                    config->task_stack_size, NULL,
                                    config->task_priority, &s_task);
    if (task_ok != pdPASS) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, connection_event_cb, NULL,
        &s_wifi_event_instance);
    if (ret != ESP_OK) {
        vTaskDelete(s_task);
        s_task = NULL;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        return ret;
    }

    ret = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, connection_event_cb, NULL,
        &s_ip_event_instance);
    if (ret != ESP_OK) {
        (void)esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
        s_wifi_event_instance = NULL;
        vTaskDelete(s_task);
        s_task = NULL;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        return ret;
    }

    s_initialized = true;
    return ESP_OK;
}

bool iot_wifi_connection_is_up(void)
{
    return s_connected;
}

esp_err_t iot_wifi_connection_wait_connected(uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t iot_wifi_connection_deinitialize(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_instance);
    const esp_err_t wifi_unregister_result =
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
    if (result == ESP_OK) {
        result = wifi_unregister_result;
    }

    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    s_wifi_event_instance = NULL;
    s_ip_event_instance = NULL;
    s_callbacks = (iot_wifi_connection_callbacks_t){0};
    s_callback_arg = NULL;
    s_retry_base_ms = 0U;
    s_retry_max_ms = 0U;
    s_retry_count = 0U;
    s_connected = false;
    s_ever_connected = false;
    s_initialized = false;
    return result;
}
