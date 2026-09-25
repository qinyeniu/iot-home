/*
 * Host-compatible HTTP adapter for IoT-Home SoftAP provisioning.
 *
 * This component does not start esp_http_server or a socket; it maps a small
 * HTTP request model to the RAM-only provisioning session and produces a JSON
 * response. Calls are not internally synchronized.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iot_provisioning_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_HTTP_MAX_BODY       4096U
#define IOT_HTTP_RESPONSE_CAPACITY 1024U

typedef struct {
    const char *method;
    const char *path;
    const char *content_type;
    const char *body;
    size_t body_len;
    uint32_t now_ms;
} iot_http_request_t;

typedef struct {
    uint16_t status_code;
    const char *content_type;
    size_t body_len;
    char body[IOT_HTTP_RESPONSE_CAPACITY + 1];
} iot_http_response_t;

typedef struct {
    iot_prov_error_t code;
    iot_prov_field_t field;
} iot_http_commit_result_t;

typedef iot_http_commit_result_t (*iot_http_commit_handler_t)(
    const iot_prov_bundle_t *bundle,
    void *context);

/**
 * Generate a NUL-terminated unique test ID shaped like tmp-<alphanumeric>.
 * Returns false if generation fails or the ID does not fit.
 */
typedef bool (*iot_http_test_id_generator_t)(
    char *test_id,
    size_t capacity,
    void *context);

typedef struct iot_http_adapter {
    bool initialized;
    iot_prov_session_t session;
    iot_http_commit_handler_t commit_handler;
    void *commit_context;
    iot_http_test_id_generator_t test_id_generator;
    void *generator_context;
    char parse_scratch[IOT_HTTP_MAX_BODY + 1];
} iot_http_adapter_t;

/**
 * Initialize the HTTP adapter and its underlying session.
 *
 * @param test_id_generator Required for POST /test.
 * @param commit_handler Optional; POST /commit returns 501 until registered.
 */
iot_prov_error_t iot_http_adapter_init(
    iot_http_adapter_t *adapter,
    const char *ap_ssid,
    const char *firmware_version,
    iot_http_test_id_generator_t test_id_generator,
    void *generator_context,
    iot_http_commit_handler_t commit_handler,
    void *commit_context);

/** Dispatch one already-received HTTP request. */
iot_prov_error_t iot_http_dispatch(
    iot_http_adapter_t *adapter,
    const iot_http_request_t *request,
    iot_http_response_t *response);

/** Register/replace the commit callback, for example during later integration. */
iot_prov_error_t iot_http_set_commit_handler(
    iot_http_adapter_t *adapter,
    iot_http_commit_handler_t handler,
    void *context);

/** Access the underlying session for Wi-Fi/MQTT result callbacks. */
iot_prov_session_t *iot_http_adapter_session(iot_http_adapter_t *adapter);

#ifdef __cplusplus
}
#endif
