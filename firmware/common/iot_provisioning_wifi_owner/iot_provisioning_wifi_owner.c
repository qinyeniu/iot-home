/*
 * Provisioning Wi-Fi owner gate implementation.
 */

#include "iot_provisioning_wifi_owner.h"

#include <stdio.h>
#include <string.h>

static size_t bounded_strlen(const char *value, size_t capacity)
{
    size_t len = 0U;

    if (value == NULL) {
        return capacity;
    }
    while (len < capacity && value[len] != '\0') {
        ++len;
    }
    return len;
}

static bool test_id_valid(const char *test_id)
{
    size_t len = bounded_strlen(test_id, IOT_PROV_TEST_ID_MAX_LEN + 1U);

    return len > 0U && len <= IOT_PROV_TEST_ID_MAX_LEN;
}

iot_wifi_owner_result_t iot_wifi_owner_init(iot_wifi_owner_state_t *state)
{
    if (state == NULL) {
        return IOT_WIFI_OWNER_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    state->initialized = true;
    state->owner = IOT_WIFI_OWNER_NORMAL;
    return IOT_WIFI_OWNER_OK;
}

iot_wifi_owner_result_t iot_wifi_owner_acquire(
    iot_wifi_owner_state_t *state,
    const char *test_id,
    uint32_t generation)
{
    if (state == NULL || !state->initialized) {
        return IOT_WIFI_OWNER_ERR_INVALID_ARG;
    }
    if (generation == 0U || !test_id_valid(test_id)) {
        return IOT_WIFI_OWNER_ERR_INVALID_ARG;
    }
    if (state->owner == IOT_WIFI_OWNER_PROVISION) {
        return IOT_WIFI_OWNER_ERR_INVALID_STATE;
    }
    if (generation <= state->provision_generation) {
        return IOT_WIFI_OWNER_ERR_GENERATION_REUSED;
    }

    state->owner = IOT_WIFI_OWNER_PROVISION;
    state->provision_generation = generation;
    (void)snprintf(state->test_id, sizeof(state->test_id), "%s", test_id);
    return IOT_WIFI_OWNER_OK;
}

iot_wifi_owner_result_t iot_wifi_owner_release(
    iot_wifi_owner_state_t *state,
    uint32_t generation)
{
    if (state == NULL || !state->initialized) {
        return IOT_WIFI_OWNER_ERR_INVALID_ARG;
    }
    if (state->owner != IOT_WIFI_OWNER_PROVISION ||
        state->provision_generation != generation) {
        return IOT_WIFI_OWNER_ERR_INVALID_STATE;
    }

    state->owner = IOT_WIFI_OWNER_NORMAL;
    memset(state->test_id, 0, sizeof(state->test_id));
    return IOT_WIFI_OWNER_OK;
}

bool iot_wifi_owner_is_provision(const iot_wifi_owner_state_t *state)
{
    return state != NULL && state->initialized &&
           state->owner == IOT_WIFI_OWNER_PROVISION;
}

bool iot_wifi_owner_normal_recovery_allowed(
    const iot_wifi_owner_state_t *state)
{
    return state != NULL && state->initialized &&
           state->owner == IOT_WIFI_OWNER_NORMAL;
}

bool iot_wifi_owner_sta_start_connect_allowed(
    const iot_wifi_owner_state_t *state)
{
    return iot_wifi_owner_normal_recovery_allowed(state);
}

bool iot_wifi_owner_event_is_current(
    const iot_wifi_owner_state_t *state,
    const char *test_id,
    uint32_t generation)
{
    if (!iot_wifi_owner_is_provision(state) ||
        generation == 0U || !test_id_valid(test_id)) {
        return false;
    }

    return state->provision_generation == generation &&
           strcmp(state->test_id, test_id) == 0;
}
