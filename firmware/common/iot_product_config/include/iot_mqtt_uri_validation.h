/*
 * Host-compatible MQTT URI validation.
 *
 * This validates shape only; it does not open a network connection and does
 * not enable TLS, WebSocket or any particular broker feature.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IOT_MQTT_URI_OK = 0,
    IOT_MQTT_URI_ERR_NULL,
    IOT_MQTT_URI_ERR_TOO_LONG,
    IOT_MQTT_URI_ERR_UNSUPPORTED_SCHEME,
    IOT_MQTT_URI_ERR_HOST,
    IOT_MQTT_URI_ERR_PORT,
    IOT_MQTT_URI_ERR_PATH,
    IOT_MQTT_URI_ERR_USERINFO,
} iot_mqtt_uri_error_t;

/**
 * Validate an MQTT broker URI.
 *
 * Supported schemes (case-insensitive): mqtt, mqtts, ws, wss. The URI must use a DNS host or
 * IPv4 literal, with an optional port. Userinfo embedded in the URI is not
 * accepted because the product manages MQTT username/password separately.
 */
iot_mqtt_uri_error_t iot_mqtt_uri_validate(const char *uri, size_t capacity);

#ifdef __cplusplus
}
#endif
