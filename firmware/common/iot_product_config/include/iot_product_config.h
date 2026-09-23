/*
 * IoT-Home product configuration store.
 *
 * Stores user-provided Wi-Fi and MQTT settings in NVS. Secrets are never
 * logged by this component. The application is responsible for deciding when
 * to enter provisioning mode and when to fall back to lab build settings.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_PRODUCT_CONFIG_NAMESPACE "iotcfg"

#define IOT_WIFI_SSID_MAX_LEN              32
#define IOT_WIFI_PASSWORD_MAX_LEN          63
#define IOT_MQTT_URI_MAX_LEN              160
#define IOT_MQTT_USERNAME_MAX_LEN          64
#define IOT_MQTT_PASSWORD_MAX_LEN          64
#define IOT_MQTT_CLIENT_ID_MAX_LEN         64

typedef struct {
    char ssid[IOT_WIFI_SSID_MAX_LEN + 1];
    char password[IOT_WIFI_PASSWORD_MAX_LEN + 1];
} iot_wifi_config_t;

typedef struct {
    char uri[IOT_MQTT_URI_MAX_LEN + 1];
    char username[IOT_MQTT_USERNAME_MAX_LEN + 1];
    char password[IOT_MQTT_PASSWORD_MAX_LEN + 1];
    char client_id[IOT_MQTT_CLIENT_ID_MAX_LEN + 1];
} iot_mqtt_config_t;

/**
 * @brief Check whether a complete Wi-Fi configuration exists.
 *
 * @param[out] present true if a valid SSID is stored.
 * @return ESP_OK on success.
 */
esp_err_t iot_product_config_has_wifi(bool *present);

/**
 * @brief Check whether a complete MQTT configuration exists.
 *
 * @param[out] present true if a valid MQTT URI is stored.
 * @return ESP_OK on success.
 */
esp_err_t iot_product_config_has_mqtt(bool *present);

/**
 * Load a stored Wi-Fi configuration; returns ESP_ERR_NOT_FOUND if absent.
 * Returns ESP_ERR_INVALID_VERSION if the stored schema is unsupported.
 */
esp_err_t iot_product_config_load_wifi(iot_wifi_config_t *config);

/**
 * Store and commit a Wi-Fi configuration. Empty passwords select an open
 * network; ordinary WPA passwords use at most 63 bytes plus NUL.
 */
esp_err_t iot_product_config_save_wifi(const iot_wifi_config_t *config);

/** Erase stored Wi-Fi keys; missing keys are not an error. */
esp_err_t iot_product_config_clear_wifi(void);

/**
 * Load a stored MQTT configuration; returns ESP_ERR_NOT_FOUND if absent.
 * Returns ESP_ERR_INVALID_VERSION if the stored schema is unsupported.
 * An empty client_id means "not configured": the application should pass
 * NULL to ESP MQTT if the default auto-generated client ID is desired.
 */
esp_err_t iot_product_config_load_mqtt(iot_mqtt_config_t *config);

/**
 * Store and commit an MQTT configuration. Empty credentials are allowed;
 * save/clear operations are internally serialized to avoid mixed keys.
 */
esp_err_t iot_product_config_save_mqtt(const iot_mqtt_config_t *config);

/** Erase stored MQTT keys; missing keys are not an error. */
esp_err_t iot_product_config_clear_mqtt(void);

#ifdef __cplusplus
}
#endif