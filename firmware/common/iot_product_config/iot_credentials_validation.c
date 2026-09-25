/*
 * Host-compatible credential validation.
 */

#include "iot_credentials_validation.h"
#include "iot_product_config_limits.h"

#include <stddef.h>
#include <stdint.h>

static void set_failed_field(iot_credentials_field_t *failed_field,
                             iot_credentials_field_t field)
{
    if (failed_field != NULL) {
        *failed_field = field;
    }
}

static bool ascii_control_char(unsigned char ch)
{
    return ch < 0x20U || ch == 0x7fU;
}

/*
 * Validate UTF-8 and reject Unicode C0/C1 controls. The input length has
 * already been bounded, so this decoder only reads bytes inside [0, len).
 */
static iot_credentials_error_t validate_utf8_text(
    const unsigned char *data,
    size_t len,
    iot_credentials_field_t field,
    iot_credentials_field_t *failed_field)
{
    size_t i = 0;

    while (i < len) {
        unsigned char first = data[i];
        uint32_t codepoint;
        size_t expected;

        if (first < 0x80U) {
            if (ascii_control_char(first)) {
                set_failed_field(failed_field, field);
                return IOT_CREDENTIALS_ERR_CONTROL_CHAR;
            }
            ++i;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            expected = 1U;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            expected = 2U;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            expected = 3U;
            codepoint = first & 0x07U;
        } else {
            set_failed_field(failed_field, field);
            return IOT_CREDENTIALS_ERR_INVALID_ENCODING;
        }

        if (i + expected >= len) {
            set_failed_field(failed_field, field);
            return IOT_CREDENTIALS_ERR_INVALID_ENCODING;
        }

        for (size_t j = 1U; j <= expected; ++j) {
            unsigned char next = data[i + j];
            if (next < 0x80U || next > 0xbfU) {
                set_failed_field(failed_field, field);
                return IOT_CREDENTIALS_ERR_INVALID_ENCODING;
            }
            codepoint = (codepoint << 6U) | (uint32_t)(next & 0x3fU);
        }

        /* Reject overlong encodings, surrogates and values above U+10FFFF. */
        if ((expected == 2U && codepoint < 0x800U) ||
            (expected == 3U && codepoint < 0x10000U) ||
            (expected == 3U && first == 0xf0U && codepoint < 0x100000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) {
            set_failed_field(failed_field, field);
            return IOT_CREDENTIALS_ERR_INVALID_ENCODING;
        }
        if (codepoint >= 0x80U && codepoint <= 0x9fU) {
            set_failed_field(failed_field, field);
            return IOT_CREDENTIALS_ERR_CONTROL_CHAR;
        }

        i += expected + 1U;
    }

    return IOT_CREDENTIALS_OK;
}

static iot_credentials_error_t validate_octet_text(
    const unsigned char *data,
    size_t len,
    iot_credentials_field_t field,
    iot_credentials_field_t *failed_field)
{
    for (size_t i = 0; i < len; ++i) {
        if (ascii_control_char(data[i])) {
            set_failed_field(failed_field, field);
            return IOT_CREDENTIALS_ERR_CONTROL_CHAR;
        }
    }
    return IOT_CREDENTIALS_OK;
}

iot_credentials_error_t iot_credentials_validate_text(
    const char *value,
    size_t capacity,
    size_t max_len,
    bool required,
    bool utf8,
    iot_credentials_field_t field,
    iot_credentials_field_t *failed_field)
{
    const unsigned char *data;
    size_t len = 0;

    set_failed_field(failed_field, IOT_CREDENTIALS_FIELD_NONE);

    if (value == NULL) {
        set_failed_field(failed_field, field);
        return IOT_CREDENTIALS_ERR_NULL;
    }
    if (capacity == 0U || max_len >= capacity) {
        set_failed_field(failed_field, field);
        return IOT_CREDENTIALS_ERR_TOO_LONG;
    }

    while (len < capacity && value[len] != '\0') {
        ++len;
    }
    if (len == capacity || len > max_len) {
        set_failed_field(failed_field, field);
        return IOT_CREDENTIALS_ERR_TOO_LONG;
    }
    if (len == 0U) {
        if (required) {
            set_failed_field(failed_field, field);
            return IOT_CREDENTIALS_ERR_EMPTY;
        }
        return IOT_CREDENTIALS_OK;
    }

    data = (const unsigned char *)value;
    return utf8 ? validate_utf8_text(data, len, field, failed_field)
                : validate_octet_text(data, len, field, failed_field);
}

iot_credentials_error_t iot_credentials_validate_wifi(
    const char *ssid,
    const char *password,
    iot_credentials_field_t *failed_field)
{
    iot_credentials_error_t err;

    err = iot_credentials_validate_text(
        ssid, IOT_WIFI_SSID_MAX_LEN + 1U, IOT_WIFI_SSID_MAX_LEN, true, false,
        IOT_CREDENTIALS_FIELD_WIFI_SSID, failed_field);
    if (err != IOT_CREDENTIALS_OK) {
        return err;
    }

    return iot_credentials_validate_text(
        password, IOT_WIFI_PASSWORD_MAX_LEN + 1U, IOT_WIFI_PASSWORD_MAX_LEN,
        false, false, IOT_CREDENTIALS_FIELD_WIFI_PASSWORD, failed_field);
}

iot_credentials_error_t iot_credentials_validate_mqtt(
    const char *uri,
    const char *username,
    const char *password,
    const char *client_id,
    iot_credentials_field_t *failed_field)
{
    iot_credentials_error_t err;

    err = iot_credentials_validate_text(
        uri, IOT_MQTT_URI_MAX_LEN + 1U, IOT_MQTT_URI_MAX_LEN, true, true,
        IOT_CREDENTIALS_FIELD_MQTT_URI, failed_field);
    if (err != IOT_CREDENTIALS_OK) {
        return err;
    }

    err = iot_credentials_validate_text(
        username, IOT_MQTT_USERNAME_MAX_LEN + 1U, IOT_MQTT_USERNAME_MAX_LEN,
        false, true, IOT_CREDENTIALS_FIELD_MQTT_USERNAME, failed_field);
    if (err != IOT_CREDENTIALS_OK) {
        return err;
    }

    err = iot_credentials_validate_text(
        password, IOT_MQTT_PASSWORD_MAX_LEN + 1U, IOT_MQTT_PASSWORD_MAX_LEN,
        false, true, IOT_CREDENTIALS_FIELD_MQTT_PASSWORD, failed_field);
    if (err != IOT_CREDENTIALS_OK) {
        return err;
    }

    return iot_credentials_validate_text(
        client_id, IOT_MQTT_CLIENT_ID_MAX_LEN + 1U, IOT_MQTT_CLIENT_ID_MAX_LEN,
        false, true, IOT_CREDENTIALS_FIELD_MQTT_CLIENT_ID, failed_field);
}
