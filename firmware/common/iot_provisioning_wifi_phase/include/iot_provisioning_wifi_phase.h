/*
 * IoT-Home provisioning Wi-Fi phase tracker.
 *
 * Host-compatible RAM-only state machine for a candidate Wi-Fi connection.
 * It classifies timeouts without calling Wi-Fi APIs or writing NVS.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "iot_provisioning_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IOT_PROV_WIFI_PHASE_STARTING = 1,
    IOT_PROV_WIFI_PHASE_SCANNING,
    IOT_PROV_WIFI_PHASE_ASSOCIATING,
    IOT_PROV_WIFI_PHASE_WAITING_IP,
    IOT_PROV_WIFI_PHASE_FINISHED,
} iot_prov_wifi_phase_t;

typedef enum {
    IOT_PROV_WIFI_PHASE_EVENT_RESET = 1,
    IOT_PROV_WIFI_PHASE_EVENT_CONNECT_REQUESTED,
    IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATION_PROGRESS,
    IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATED,
    IOT_PROV_WIFI_PHASE_EVENT_GOT_IP,
    IOT_PROV_WIFI_PHASE_EVENT_DISCONNECTED,
} iot_prov_wifi_phase_event_t;

typedef struct {
    bool initialized;
    iot_prov_wifi_phase_t phase;
} iot_prov_wifi_phase_state_t;

typedef enum {
    IOT_PROV_WIFI_PHASE_OK = 0,
    IOT_PROV_WIFI_PHASE_ERR_INVALID_ARG,
    IOT_PROV_WIFI_PHASE_ERR_INVALID_STATE,
} iot_prov_wifi_phase_result_t;

iot_prov_wifi_phase_result_t iot_prov_wifi_phase_init(
    iot_prov_wifi_phase_state_t *state);

iot_prov_wifi_phase_result_t iot_prov_wifi_phase_apply(
    iot_prov_wifi_phase_state_t *state,
    iot_prov_wifi_phase_event_t event);

iot_prov_wifi_phase_t iot_prov_wifi_phase_get(
    const iot_prov_wifi_phase_state_t *state);

/*
 * Map a timeout in the current phase to the session error.
 * SCANNING/ASSOCIATING -> WIFI_ASSOC_TIMEOUT; WAITING_IP -> NO_IP_ADDRESS.
 */
iot_prov_error_t iot_prov_wifi_phase_timeout_error(
    const iot_prov_wifi_phase_state_t *state);

#ifdef __cplusplus
}
#endif
