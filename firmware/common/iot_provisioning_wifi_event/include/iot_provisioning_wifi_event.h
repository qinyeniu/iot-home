/*
 * IoT-Home provisioning Wi-Fi event adapter.
 *
 * Host-compatible RAM-only adapter that converts a raw Wi-Fi got-ip or
 * disconnected event into one WIFI_EVENT command for the provisioning queue.
 * It does not register event handlers, call Wi-Fi APIs or write NVS.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "iot_provisioning_command.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Numeric values match ESP-IDF Wi-Fi disconnect reason codes. */
typedef enum {
    IOT_PROV_WIFI_REASON_BEACON_TIMEOUT = 200,
    IOT_PROV_WIFI_REASON_NO_AP_FOUND = 201,
    IOT_PROV_WIFI_REASON_AUTH_FAIL = 202,
    IOT_PROV_WIFI_REASON_ASSOC_FAIL = 203,
    IOT_PROV_WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
    IOT_PROV_WIFI_REASON_CONNECTION_FAIL = 205,
} iot_prov_wifi_reason_code_t;

typedef enum {
    IOT_PROV_WIFI_INPUT_DISCONNECTED = 1,
    IOT_PROV_WIFI_INPUT_GOT_IP,
} iot_prov_wifi_input_type_t;

typedef struct {
    iot_prov_wifi_input_type_t type;

    /* Used only when type is IOT_PROV_WIFI_INPUT_DISCONNECTED. */
    uint8_t reason;
} iot_prov_wifi_input_t;

typedef struct {
    iot_prov_command_t command;
    iot_prov_error_t error;
} iot_prov_wifi_event_output_t;

typedef enum {
    IOT_PROV_WIFI_EVENT_ADAPTER_OK = 0,
    IOT_PROV_WIFI_EVENT_ADAPTER_ERR_INVALID_ARG,
} iot_prov_wifi_event_adapter_result_t;

/*
 * Build a serialized WIFI_EVENT queue command.
 *
 * For GOT_IP, output.error is IOT_PROV_OK. For DISCONNECTED, it maps the raw
 * reason to WIFI_AUTH_FAILED, WIFI_ASSOC_TIMEOUT or INTERNAL. The caller must
 * still discard the command if its owner generation/test_id is stale.
 */
iot_prov_wifi_event_adapter_result_t iot_prov_wifi_event_build(
    const char *test_id,
    uint32_t owner_generation,
    const iot_prov_wifi_input_t *input,
    iot_prov_wifi_event_output_t *output);

#ifdef __cplusplus
}
#endif
