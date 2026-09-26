/*
 * Provisioning Wi-Fi phase tracker implementation.
 */

#include "iot_provisioning_wifi_phase.h"

#include <string.h>

iot_prov_wifi_phase_result_t iot_prov_wifi_phase_init(
    iot_prov_wifi_phase_state_t *state)
{
    if (state == NULL) {
        return IOT_PROV_WIFI_PHASE_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    state->initialized = true;
    state->phase = IOT_PROV_WIFI_PHASE_STARTING;
    return IOT_PROV_WIFI_PHASE_OK;
}

static iot_prov_wifi_phase_result_t set_phase(
    iot_prov_wifi_phase_state_t *state,
    iot_prov_wifi_phase_t phase)
{
    state->phase = phase;
    return IOT_PROV_WIFI_PHASE_OK;
}

iot_prov_wifi_phase_result_t iot_prov_wifi_phase_apply(
    iot_prov_wifi_phase_state_t *state,
    iot_prov_wifi_phase_event_t event)
{
    if (state == NULL || !state->initialized) {
        return IOT_PROV_WIFI_PHASE_ERR_INVALID_ARG;
    }
    if (event < IOT_PROV_WIFI_PHASE_EVENT_RESET ||
        event > IOT_PROV_WIFI_PHASE_EVENT_DISCONNECTED) {
        return IOT_PROV_WIFI_PHASE_ERR_INVALID_ARG;
    }

    if (event == IOT_PROV_WIFI_PHASE_EVENT_RESET) {
        return set_phase(state, IOT_PROV_WIFI_PHASE_STARTING);
    }

    switch (state->phase) {
        case IOT_PROV_WIFI_PHASE_STARTING:
            if (event == IOT_PROV_WIFI_PHASE_EVENT_CONNECT_REQUESTED) {
                return set_phase(state, IOT_PROV_WIFI_PHASE_SCANNING);
            }
            break;

        case IOT_PROV_WIFI_PHASE_SCANNING:
            if (event == IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATION_PROGRESS) {
                return set_phase(state, IOT_PROV_WIFI_PHASE_ASSOCIATING);
            }
            if (event == IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATED) {
                return set_phase(state, IOT_PROV_WIFI_PHASE_WAITING_IP);
            }
            if (event == IOT_PROV_WIFI_PHASE_EVENT_CONNECT_REQUESTED) {
                return IOT_PROV_WIFI_PHASE_OK;
            }
            break;

        case IOT_PROV_WIFI_PHASE_ASSOCIATING:
            if (event == IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATED) {
                return set_phase(state, IOT_PROV_WIFI_PHASE_WAITING_IP);
            }
            if (event == IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATION_PROGRESS) {
                return IOT_PROV_WIFI_PHASE_OK;
            }
            break;

        case IOT_PROV_WIFI_PHASE_WAITING_IP:
            if (event == IOT_PROV_WIFI_PHASE_EVENT_GOT_IP) {
                return set_phase(state, IOT_PROV_WIFI_PHASE_FINISHED);
            }
            break;

        case IOT_PROV_WIFI_PHASE_FINISHED:
            break;
    }

    if (event == IOT_PROV_WIFI_PHASE_EVENT_DISCONNECTED &&
        state->phase != IOT_PROV_WIFI_PHASE_FINISHED) {
        return set_phase(state, IOT_PROV_WIFI_PHASE_STARTING);
    }
    return IOT_PROV_WIFI_PHASE_ERR_INVALID_STATE;
}

iot_prov_wifi_phase_t iot_prov_wifi_phase_get(
    const iot_prov_wifi_phase_state_t *state)
{
    if (state == NULL || !state->initialized) {
        return IOT_PROV_WIFI_PHASE_STARTING;
    }
    return state->phase;
}

iot_prov_error_t iot_prov_wifi_phase_timeout_error(
    const iot_prov_wifi_phase_state_t *state)
{
    iot_prov_wifi_phase_t phase;

    if (state == NULL || !state->initialized) {
        return IOT_PROV_ERR_INVALID_ARG;
    }

    phase = state->phase;
    switch (phase) {
        case IOT_PROV_WIFI_PHASE_SCANNING:
        case IOT_PROV_WIFI_PHASE_ASSOCIATING:
            return IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT;

        case IOT_PROV_WIFI_PHASE_WAITING_IP:
            return IOT_PROV_ERR_WIFI_NO_IP_ADDRESS;

        case IOT_PROV_WIFI_PHASE_STARTING:
        case IOT_PROV_WIFI_PHASE_FINISHED:
            return IOT_PROV_ERR_INVALID_STATE;
    }
    return IOT_PROV_ERR_INTERNAL;
}

