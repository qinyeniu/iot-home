#include "iot_mqtt_client.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"

struct iot_mqtt_client {
    esp_mqtt_client_handle_t handle;
    iot_mqtt_client_callbacks_t callbacks;
    void *callback_arg;
    volatile bool connected;
    bool started;
};

static const char *TAG = "iot_mqtt";

static void invoke_connected(iot_mqtt_client_t *client)
{
    if (client->callbacks.on_connected != NULL) {
        client->callbacks.on_connected(client->callback_arg);
    }
}

static void invoke_disconnected(iot_mqtt_client_t *client)
{
    if (client->callbacks.on_disconnected != NULL) {
        client->callbacks.on_disconnected(client->callback_arg);
    }
}

static void invoke_data(iot_mqtt_client_t *client,
                        const char *topic,
                        const char *data)
{
    if (client->callbacks.on_data != NULL) {
        client->callbacks.on_data(topic, data, client->callback_arg);
    }
}

static void mqtt_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    iot_mqtt_client_t *client = arg;
    esp_mqtt_event_handle_t event = event_data;

    (void)event_base;

    if (client == NULL || event == NULL) {
        return;
    }

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        client->connected = true;
        invoke_connected(client);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        client->connected = false;
        invoke_disconnected(client);
        break;

    case MQTT_EVENT_DATA: {
        const size_t topic_size = (size_t)event->topic_len + 1U;
        const size_t data_size = (size_t)event->data_len + 1U;
        char *topic = calloc(topic_size, 1U);
        char *data = calloc(data_size, 1U);

        if (topic == NULL || data == NULL) {
            ESP_LOGE(TAG, "No memory for MQTT data event");
            free(topic);
            free(data);
            break;
        }

        memcpy(topic, event->topic, (size_t)event->topic_len);
        memcpy(data, event->data, (size_t)event->data_len);
        ESP_LOGI(TAG, "MQTT rx: %s = %s", topic, data);
        invoke_data(client, topic, data);
        free(topic);
        free(data);
        break;
    }

    default:
        break;
    }
}

esp_err_t iot_mqtt_client_create(
    const iot_mqtt_client_config_t *config,
    const iot_mqtt_client_callbacks_t *callbacks,
    void *callback_arg,
    iot_mqtt_client_t **client)
{
    if (config == NULL || callbacks == NULL || client == NULL ||
        config->uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    iot_mqtt_client_t *instance = calloc(1U, sizeof(*instance));
    if (instance == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = config->uri,
        .credentials.username = config->username,
        .credentials.authentication.password = config->password,
        .credentials.client_id = config->client_id,
        .session.last_will = {
            .topic = config->last_will.topic,
            .msg = config->last_will.message,
            .msg_len = config->last_will.message_len,
            .qos = config->last_will.qos,
            .retain = config->last_will.retain,
        },
    };

    esp_mqtt_client_handle_t handle = esp_mqtt_client_init(&mqtt_config);
    if (handle == NULL) {
        free(instance);
        return ESP_FAIL;
    }

    instance->handle = handle;
    instance->callbacks = *callbacks;
    instance->callback_arg = callback_arg;
    *client = instance;
    return ESP_OK;
}

esp_err_t iot_mqtt_client_start(iot_mqtt_client_t *client)
{
    if (client == NULL || client->handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_mqtt_client_register_event(
        client->handle, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_mqtt_client_start(client->handle);
    if (err != ESP_OK) {
        return err;
    }

    client->started = true;
    return ESP_OK;
}

esp_err_t iot_mqtt_client_stop(iot_mqtt_client_t *client)
{
    if (client == NULL || client->handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!client->started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_mqtt_client_stop(client->handle);
    if (err != ESP_OK) {
        return err;
    }

    client->started = false;
    client->connected = false;
    return ESP_OK;
}

esp_err_t iot_mqtt_client_destroy(iot_mqtt_client_t *client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    if (client->started) {
        result = iot_mqtt_client_stop(client);
        if (result != ESP_OK) {
            return result;
        }
    }

    if (client->handle != NULL) {
        result = esp_mqtt_client_destroy(client->handle);
    }
    free(client);
    return result;
}

bool iot_mqtt_client_is_connected(const iot_mqtt_client_t *client)
{
    return client != NULL && client->connected;
}

bool iot_mqtt_client_publish(iot_mqtt_client_t *client,
                             const char *topic,
                             const char *data,
                             int qos,
                             bool retain)
{
    if (client == NULL || client->handle == NULL || topic == NULL) {
        return false;
    }

    const int message_id = esp_mqtt_client_publish(
        client->handle, topic, data, 0, qos, retain ? 1 : 0);
    return message_id >= 0;
}

esp_err_t iot_mqtt_client_subscribe(iot_mqtt_client_t *client,
                                    const char *topic_filter,
                                    int qos)
{
    if (client == NULL || client->handle == NULL || topic_filter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int message_id = esp_mqtt_client_subscribe(
        client->handle, topic_filter, qos);
    return message_id >= 0 ? ESP_OK : ESP_FAIL;
}