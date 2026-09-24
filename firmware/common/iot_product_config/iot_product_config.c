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

static esp_err_t config_public_error(esp_err_t err)
{
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
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
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
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
        return config_public_error(err);
    }

    err = config_check_schema_locked(handle);
    if (err != ESP_OK) {
        nvs_close(handle);
        return config_public_error(err);
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
    if (err == ESP_OK &&
        !config_is_valid_nonempty(config->ssid, IOT_WIFI_SSID_MAX_LEN)) {
        err = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    return config_public_error(err);
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
        return config_public_error(err);
    }

    err = config_check_schema_locked(handle);
    if (err != ESP_OK) {
        nvs_close(handle);
        return config_public_error(err);
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
    if (err == ESP_OK &&
        !config_is_valid_nonempty(config->uri, IOT_MQTT_URI_MAX_LEN)) {
        err = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    return config_public_error(err);
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

/* ===================== Complete A/B bundle storage ===================== */

#define BUNDLE_MAGIC         0x494F4354u  /* "IOTC" */
#define BUNDLE_SCHEMA        1u
#define KEY_BUNDLE_A         "cfg_a"
#define KEY_BUNDLE_B         "cfg_b"
#define KEY_BUNDLE_MARKER    "cfg_commit"

typedef struct __attribute__((packed)) {
    uint16_t schema_version;
    char wifi_ssid[IOT_WIFI_SSID_MAX_LEN + 1];
    char wifi_password[IOT_WIFI_PASSWORD_MAX_LEN + 1];
    char mqtt_uri[IOT_MQTT_URI_MAX_LEN + 1];
    char mqtt_username[IOT_MQTT_USERNAME_MAX_LEN + 1];
    char mqtt_password[IOT_MQTT_PASSWORD_MAX_LEN + 1];
    char mqtt_client_id[IOT_MQTT_CLIENT_ID_MAX_LEN + 1];
} bundle_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t schema_version;
    uint8_t active_slot;       /* 0 = A, 1 = B */
    uint16_t payload_len;
    uint32_t generation;
    uint32_t payload_crc;
} bundle_marker_t;

_Static_assert(sizeof(bundle_payload_t) == 455U, "bundle payload size changed");
_Static_assert(sizeof(bundle_marker_t) == 17U, "bundle marker size changed");

static uint32_t bundle_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffU;

    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1) ^ 0xedb88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xffffffffU;
}

static void bundle_copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t len = strnlen(src, dst_size - 1U);

    memset(dst, 0, dst_size);
    memcpy(dst, src, len);
}

static bool bundle_inputs_valid(const iot_wifi_config_t *wifi,
                                const iot_mqtt_config_t *mqtt)
{
    return wifi != NULL && mqtt != NULL &&
        config_is_valid_nonempty(wifi->ssid, IOT_WIFI_SSID_MAX_LEN) &&
        config_is_valid_text(wifi->password, IOT_WIFI_PASSWORD_MAX_LEN) &&
        config_is_valid_nonempty(mqtt->uri, IOT_MQTT_URI_MAX_LEN) &&
        config_is_valid_text(mqtt->username, IOT_MQTT_USERNAME_MAX_LEN) &&
        config_is_valid_text(mqtt->password, IOT_MQTT_PASSWORD_MAX_LEN) &&
        config_is_valid_text(mqtt->client_id, IOT_MQTT_CLIENT_ID_MAX_LEN);
}

static esp_err_t bundle_build(const iot_wifi_config_t *wifi,
                              const iot_mqtt_config_t *mqtt,
                              bundle_payload_t *payload,
                              uint32_t *crc)
{
    memset(payload, 0, sizeof(*payload));
    payload->schema_version = BUNDLE_SCHEMA;
    bundle_copy_text(payload->wifi_ssid, sizeof(payload->wifi_ssid), wifi->ssid);
    bundle_copy_text(payload->wifi_password, sizeof(payload->wifi_password),
                     wifi->password);
    bundle_copy_text(payload->mqtt_uri, sizeof(payload->mqtt_uri), mqtt->uri);
    bundle_copy_text(payload->mqtt_username, sizeof(payload->mqtt_username),
                     mqtt->username);
    bundle_copy_text(payload->mqtt_password, sizeof(payload->mqtt_password),
                     mqtt->password);
    bundle_copy_text(payload->mqtt_client_id, sizeof(payload->mqtt_client_id),
                     mqtt->client_id);
    *crc = bundle_crc32((const uint8_t *)payload, sizeof(*payload));
    return ESP_OK;
}

static bool bundle_payload_valid(const bundle_payload_t *payload)
{
    return payload->schema_version == BUNDLE_SCHEMA &&
        config_is_valid_nonempty(payload->wifi_ssid, IOT_WIFI_SSID_MAX_LEN) &&
        config_is_valid_text(payload->wifi_password,
                             IOT_WIFI_PASSWORD_MAX_LEN) &&
        config_is_valid_nonempty(payload->mqtt_uri, IOT_MQTT_URI_MAX_LEN) &&
        config_is_valid_text(payload->mqtt_username,
                             IOT_MQTT_USERNAME_MAX_LEN) &&
        config_is_valid_text(payload->mqtt_password,
                             IOT_MQTT_PASSWORD_MAX_LEN) &&
        config_is_valid_text(payload->mqtt_client_id,
                             IOT_MQTT_CLIENT_ID_MAX_LEN);
}

static bool bundle_marker_valid(const bundle_marker_t *marker)
{
    return marker->magic == BUNDLE_MAGIC &&
        marker->schema_version == BUNDLE_SCHEMA &&
        marker->active_slot <= 1U &&
        marker->payload_len == (uint16_t)sizeof(bundle_payload_t);
}

static const char *bundle_key_for_slot(uint8_t slot)
{
    return slot == 0U ? KEY_BUNDLE_A : KEY_BUNDLE_B;
}

static esp_err_t bundle_load_marker(nvs_handle_t handle,
                                    bundle_marker_t *marker)
{
    size_t required = sizeof(*marker);
    esp_err_t err = nvs_get_blob(handle, KEY_BUNDLE_MARKER, marker, &required);

    if (err != ESP_OK) {
        return config_public_error(err);
    }
    if (required != sizeof(*marker) || !bundle_marker_valid(marker)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t iot_product_config_save_bundle(const iot_wifi_config_t *wifi,
                                         const iot_mqtt_config_t *mqtt)
{
    nvs_handle_t handle;
    bundle_marker_t old_marker;
    bundle_marker_t new_marker;
    bundle_payload_t payload;
    uint8_t target_slot;
    uint32_t crc;
    bool have_old = false;
    esp_err_t err;

    if (!bundle_inputs_valid(wifi, mqtt)) {
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

    err = bundle_load_marker(handle, &old_marker);
    if (err == ESP_OK) {
        have_old = true;
    } else if (err != ESP_ERR_NOT_FOUND) {
        nvs_close(handle);
        config_lock_give();
        return err;
    }

    target_slot = have_old ? (uint8_t)(old_marker.active_slot ^ 1U) : 0U;
    bundle_build(wifi, mqtt, &payload, &crc);

    err = nvs_set_blob(handle, bundle_key_for_slot(target_slot), &payload,
                       sizeof(payload));
    if (err != ESP_OK) {
        nvs_close(handle);
        config_lock_give();
        return err;
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        nvs_close(handle);
        config_lock_give();
        return err;
    }

    memset(&new_marker, 0, sizeof(new_marker));
    new_marker.magic = BUNDLE_MAGIC;
    new_marker.schema_version = BUNDLE_SCHEMA;
    new_marker.active_slot = target_slot;
    new_marker.payload_len = (uint16_t)sizeof(payload);
    new_marker.generation = have_old ? old_marker.generation + 1U : 1U;
    new_marker.payload_crc = crc;

    err = nvs_set_blob(handle, KEY_BUNDLE_MARKER, &new_marker,
                       sizeof(new_marker));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    config_lock_give();
    return err;
}

esp_err_t iot_product_config_load_bundle(iot_wifi_config_t *wifi,
                                         iot_mqtt_config_t *mqtt)
{
    nvs_handle_t handle;
    bundle_marker_t marker;
    bundle_payload_t payload;
    size_t required = sizeof(payload);
    const char *data_key;
    esp_err_t err;

    if (wifi == NULL || mqtt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(wifi, 0, sizeof(*wifi));
    memset(mqtt, 0, sizeof(*mqtt));

    err = config_open_readonly(&handle);
    if (err != ESP_OK) {
        return config_public_error(err);
    }
    err = bundle_load_marker(handle, &marker);
    if (err != ESP_OK) {
        nvs_close(handle);
        return config_public_error(err);
    }

    data_key = bundle_key_for_slot(marker.active_slot);
    err = nvs_get_blob(handle, data_key, &payload, &required);
    if (err != ESP_OK) {
        nvs_close(handle);
        return config_public_error(err);
    }
    if (required != sizeof(payload) ||
        bundle_crc32((const uint8_t *)&payload, sizeof(payload)) !=
            marker.payload_crc ||
        !bundle_payload_valid(&payload)) {
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(wifi->ssid, payload.wifi_ssid, sizeof(wifi->ssid));
    memcpy(wifi->password, payload.wifi_password, sizeof(wifi->password));
    memcpy(mqtt->uri, payload.mqtt_uri, sizeof(mqtt->uri));
    memcpy(mqtt->username, payload.mqtt_username, sizeof(mqtt->username));
    memcpy(mqtt->password, payload.mqtt_password, sizeof(mqtt->password));
    memcpy(mqtt->client_id, payload.mqtt_client_id, sizeof(mqtt->client_id));

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t iot_product_config_clear_bundle(void)
{
    static const char *keys[] = {
        KEY_BUNDLE_A, KEY_BUNDLE_B, KEY_BUNDLE_MARKER,
    };
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

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        esp_err_t erase_err = nvs_erase_key(handle, keys[i]);
        if (erase_err == ESP_ERR_NVS_NOT_FOUND) {
            erase_err = ESP_OK;
        }
        if (erase_err != ESP_OK) {
            err = erase_err;
            break;
        }
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    config_lock_give();
    return err;
}
