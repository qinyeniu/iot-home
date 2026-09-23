/*
 * IoT-Home product configuration store implementation.
 * Values are written to NVS only when the user explicitly saves them.
 */

#include "iot_product_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_SCHEMA_VERSION 1

#define KEY_VERSION   "version"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_MQTT_URI  "mqtt_uri"
#define KEY_MQTT_USER "mqtt_user"
#define KEY_MQTT_PASS "mqtt_pass"
#define KEY_MQTT_CID  "mqtt_cid"

static bool config_is_valid_nonempty(const char *value, size_t max_len)
{
    size_t len;

    if (value == NULL) {
        return false;
    }
    len = strnlen(value, max_len + 1U);
    return len > 0U && len <= max_len;
}

static bool config_is_valid_text(const char *value, size_t max_len)
{
    size_t len;

    if (value == NULL) {
        return false;
    }
    len = strnlen(value, max_len + 1U);
    return len <= max_len;
}

static esp_err_t config_get_str(nvs_handle_t handle, const char *key,
                                char *out, size_t out_size)
{
    size_t required = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required);
    if (err != ESP_OK) {
        return err;
    }
    if (required > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return nvs_get_str(handle, key, out, &required);
}

static esp_err_t config_ensure_schema(nvs_handle_t handle)
{
    uint8_t version = 0;
    esp_err_t err = nvs_get_u8(handle, KEY_VERSION, &version);
    if (err == ESP_OK && version == CONFIG_SCHEMA_VERSION) {
        return ESP_OK;
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    err = nvs_set_u8(handle, KEY_VERSION, CONFIG_SCHEMA_VERSION);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    return err;
}

static SemaphoreHandle_t config_write_lock;

static esp_err_t config_open(nvs_handle_t *handle)
{
    return nvs_open(IOT_PRODUCT_CONFIG_NAMESPACE, NVS_READWRITE, handle);
}

static esp_err_t config_open_readonly(nvs_handle_t *handle)
{
    return nvs_open(IOT_PRODUCT_CONFIG_NAMESPACE, NVS_READONLY, handle);
}

static bool config_lock_take(void)
{
    if (config_write_lock == NULL) {
        config_write_lock = xSemaphoreCreateMutex();
        if (config_write_lock == NULL) {
            return false;
        }
    }
    return xSemaphoreTake(config_write_lock, portMAX_DELAY) == pdTRUE;
}

static void config_lock_give(void)
{
    xSemaphoreGive(config_write_lock);
}

static esp_err_t config_check_schema_locked(nvs_handle_t handle)
{
    uint8_t version = 0;
    esp_err_t err = nvs_get_u8(handle, KEY_VERSION, &version);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    return version == CONFIG_SCHEMA_VERSION ? ESP_OK : ESP_ERR_INVALID_VERSION;
}

esp_err_t iot_product_config_has_wifi(bool *present)
{
    nvs_handle_t handle;
    size_t required = 0;
    esp_err_t err;

    if (present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;

    err = nvs_open(IOT_PRODUCT_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, KEY_WIFI_SSID, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        *present = required > 1U && required <= IOT_WIFI_SSID_MAX_LEN + 1U;
    }
    nvs_close(handle);
    return err == ESP_OK ? ESP_OK : err;
}

esp_err_t iot_product_config_has_mqtt(bool *present)
{
    nvs_handle_t handle;
    size_t required = 0;
    esp_err_t err;

    if (present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;

    err = nvs_open(IOT_PRODUCT_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, KEY_MQTT_URI, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        *present = required > 1U && required <= IOT_MQTT_URI_MAX_LEN + 1U;
    }
    nvs_close(handle);
    return err == ESP_OK ? ESP_OK : err;
}

esp_err_t iot_product_config_load_wifi(iot_wifi_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));

    err = config_open_readonly(&handle);
    if (err != ESP_OK) {
        return err;
    }

    err = config_check_schema_locked(handle);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    // SSID is mandatory; password is optional and defaults to empty.
    err = config_get_str(handle, KEY_WIFI_SSID, config->ssid,
                         sizeof(config->ssid));
    if (err == ESP_OK) {
        err = config_get_str(handle, KEY_WIFI_PASS, config->password,
                             sizeof(config->password));
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    nvs_close(handle);
    return err;
}

esp_err_t iot_product_config_save_wifi(const iot_wifi_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (config == NULL ||
        !config_is_valid_nonempty(config->ssid, IOT_WIFI_SSID_MAX_LEN) ||
        !config_is_valid_text(config->password, IOT_WIFI_PASSWORD_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config_lock_take()) {
        return ESP_ERR_NO_MEM;
    }

    err = config_open(&handle);
    if (err != ESP_OK) {
        config_lock_give();
        return err;
    }
    err = config_ensure_schema(handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WIFI_SSID, config->ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WIFI_PASS, config->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    config_lock_give();
    return err;
}

esp_err_t iot_product_config_clear_wifi(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (!config_lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    err = config_open(&handle);
    if (err != ESP_OK) {
        config_lock_give();
        return err;
    }

    err = nvs_erase_key(handle, KEY_WIFI_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, KEY_WIFI_PASS);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    config_lock_give();
    return err;
}

esp_err_t iot_product_config_load_mqtt(iot_mqtt_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));

    err = config_open_readonly(&handle);
    if (err != ESP_OK) {
        return err;
    }

    err = config_check_schema_locked(handle);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    // URI is mandatory. Credentials and client ID are optional and default
    // to empty; the application passes NULL client ID when it wants ESP MQTT
    // to use its default auto-generated identifier.
    err = config_get_str(handle, KEY_MQTT_URI, config->uri,
                         sizeof(config->uri));
    if (err == ESP_OK) {
        err = config_get_str(handle, KEY_MQTT_USER, config->username,
                             sizeof(config->username));
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = config_get_str(handle, KEY_MQTT_PASS, config->password,
                             sizeof(config->password));
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = config_get_str(handle, KEY_MQTT_CID, config->client_id,
                             sizeof(config->client_id));
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    nvs_close(handle);
    return err;
}

esp_err_t iot_product_config_save_mqtt(const iot_mqtt_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (config == NULL ||
        !config_is_valid_nonempty(config->uri, IOT_MQTT_URI_MAX_LEN) ||
        !config_is_valid_text(config->username, IOT_MQTT_USERNAME_MAX_LEN) ||
        !config_is_valid_text(config->password, IOT_MQTT_PASSWORD_MAX_LEN) ||
        !config_is_valid_text(config->client_id, IOT_MQTT_CLIENT_ID_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config_lock_take()) {
        return ESP_ERR_NO_MEM;
    }

    err = config_open(&handle);
    if (err != ESP_OK) {
        config_lock_give();
        return err;
    }
    err = config_ensure_schema(handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_MQTT_URI, config->uri);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_MQTT_USER, config->username);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_MQTT_PASS, config->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_MQTT_CID, config->client_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    config_lock_give();
    return err;
}

esp_err_t iot_product_config_clear_mqtt(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (!config_lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    err = config_open(&handle);
    if (err != ESP_OK) {
        config_lock_give();
        return err;
    }

    const char *keys[] = {KEY_MQTT_URI, KEY_MQTT_USER, KEY_MQTT_PASS, KEY_MQTT_CID};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        err = nvs_erase_key(handle, keys[i]);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK) {
            break;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    config_lock_give();
    return err;
}
