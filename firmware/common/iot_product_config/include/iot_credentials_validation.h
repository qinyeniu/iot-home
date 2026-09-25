/*
 * Host-compatible validation for user-provided Wi-Fi and MQTT credentials.
 *
 * The validator never logs field values. Callers receive a machine-readable
 * field/reason, suitable for returning to a local provisioning page without
 * exposing secrets.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IOT_CREDENTIALS_OK = 0,
    IOT_CREDENTIALS_ERR_NULL,
    IOT_CREDENTIALS_ERR_EMPTY,
    IOT_CREDENTIALS_ERR_TOO_LONG,
    IOT_CREDENTIALS_ERR_CONTROL_CHAR,
    IOT_CREDENTIALS_ERR_INVALID_ENCODING,
} iot_credentials_error_t;

typedef enum {
    IOT_CREDENTIALS_FIELD_NONE = 0,
    IOT_CREDENTIALS_FIELD_WIFI_SSID,
    IOT_CREDENTIALS_FIELD_WIFI_PASSWORD,
    IOT_CREDENTIALS_FIELD_MQTT_URI,
    IOT_CREDENTIALS_FIELD_MQTT_USERNAME,
    IOT_CREDENTIALS_FIELD_MQTT_PASSWORD,
    IOT_CREDENTIALS_FIELD_MQTT_CLIENT_ID,
} iot_credentials_field_t;

/**
 * Validate one bounded text field.
 *
 * @param value Field buffer; NULL is a caller error.
 * @param capacity Number of bytes readable from @p value, including NUL.
 * @param max_len Maximum logical field length, excluding NUL.
 * @param required true if an empty string is an error.
 * @param utf8 true for MQTT text fields; false treats Wi-Fi SSID/password as
 *              an octet string while still rejecting ASCII C0/DEL controls.
 */
iot_credentials_error_t iot_credentials_validate_text(
    const char *value,
    size_t capacity,
    size_t max_len,
    bool required,
    bool utf8,
    iot_credentials_field_t field,
    iot_credentials_field_t *failed_field);

/**
 * Validate Wi-Fi fields. SSID is required; an empty password selects an open
 * network. Product callers must pass fixed buffers containing at least
 * MAX_LEN + 1 readable bytes.
 */
iot_credentials_error_t iot_credentials_validate_wifi(
    const char *ssid,
    const char *password,
    iot_credentials_field_t *failed_field);

/**
 * Validate MQTT fields. URI is required; username, password and client ID may
 * be empty. MQTT text fields must be valid UTF-8 and must not contain Unicode
 * C0/C1 control characters.
 */
iot_credentials_error_t iot_credentials_validate_mqtt(
    const char *uri,
    const char *username,
    const char *password,
    const char *client_id,
    iot_credentials_field_t *failed_field);

#ifdef __cplusplus
}
#endif
