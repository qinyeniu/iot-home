/*
 * Normal Wi-Fi recovery quiesce coordinator implementation.
 */

#include "iot_provisioning_recovery_quiesce.h"

#include <string.h>

iot_recovery_quiesce_result_t iot_recovery_quiesce_init(
    iot_recovery_quiesce_t *state)
{
    if (state == NULL) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    state->initialized = true;
    return IOT_RECOVERY_QUIESCE_OK;
}

iot_recovery_quiesce_result_t iot_recovery_quiesce_begin_operation(
    iot_recovery_quiesce_t *state,
    uint32_t operation_id)
{
    if (state == NULL || !state->initialized) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (operation_id == 0U) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (operation_id <= state->last_operation_id) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (state->quiesce_requested) {
        return IOT_RECOVERY_QUIESCE_BUSY;
    }
    if (state->operation_active || state->destructive_tail_active) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE;
    }

    state->operation_active = true;
    state->operation_id = operation_id;
    return IOT_RECOVERY_QUIESCE_OK;
}

iot_recovery_quiesce_result_t iot_recovery_quiesce_end_operation(
    iot_recovery_quiesce_t *state,
    uint32_t operation_id,
    bool request_destructive_tail,
    bool *destructive_tail_active)
{
    if (state == NULL || !state->initialized || destructive_tail_active == NULL) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (!state->operation_active || state->operation_id != operation_id) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE;
    }

    *destructive_tail_active = false;
    state->last_operation_id = state->operation_id;
    state->operation_active = false;
    state->operation_id = 0U;

    if (!state->quiesce_requested && request_destructive_tail) {
        state->destructive_tail_active = true;
        state->destructive_tail_id = operation_id;
        *destructive_tail_active = true;
    }
    return IOT_RECOVERY_QUIESCE_OK;
}

iot_recovery_quiesce_result_t iot_recovery_quiesce_end_destructive_tail(
    iot_recovery_quiesce_t *state,
    uint32_t tail_id)
{
    if (state == NULL || !state->initialized) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (!state->destructive_tail_active ||
        state->destructive_tail_id != tail_id) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE;
    }

    state->destructive_tail_active = false;
    state->destructive_tail_id = 0U;
    return IOT_RECOVERY_QUIESCE_OK;
}

iot_recovery_quiesce_result_t iot_recovery_quiesce_request(
    iot_recovery_quiesce_t *state,
    uint32_t request_id)
{
    if (state == NULL || !state->initialized) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (request_id == 0U) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (state->quiesce_requested && state->quiesce_request_id != request_id) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE;
    }

    if (state->operation_active || state->destructive_tail_active) {
        state->quiesce_requested = true;
        state->quiesce_request_id = request_id;
        return IOT_RECOVERY_QUIESCE_BUSY;
    }

    state->quiesce_requested = true;
    state->quiesce_request_id = request_id;
    return IOT_RECOVERY_QUIESCE_OK;
}

iot_recovery_quiesce_result_t iot_recovery_quiesce_cancel_request(
    iot_recovery_quiesce_t *state,
    uint32_t request_id)
{
    if (state == NULL || !state->initialized) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (request_id == 0U) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (!state->quiesce_requested || state->quiesce_request_id != request_id) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE;
    }
    if (state->operation_active || state->destructive_tail_active) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE;
    }

    state->quiesce_requested = false;
    state->quiesce_request_id = 0U;
    return IOT_RECOVERY_QUIESCE_OK;
}

iot_recovery_quiesce_result_t iot_recovery_quiesce_reset(
    iot_recovery_quiesce_t *state)
{
    if (state == NULL || !state->initialized) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG;
    }
    if (state->operation_active || state->destructive_tail_active) {
        return IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE;
    }

    state->quiesce_requested = false;
    state->quiesce_request_id = 0U;
    state->operation_id = 0U;
    state->destructive_tail_id = 0U;
    return IOT_RECOVERY_QUIESCE_OK;
}

bool iot_recovery_quiesce_operation_active(
    const iot_recovery_quiesce_t *state)
{
    return state != NULL && state->initialized && state->operation_active;
}

bool iot_recovery_quiesce_destructive_tail_active(
    const iot_recovery_quiesce_t *state)
{
    return state != NULL && state->initialized &&
           state->destructive_tail_active;
}

bool iot_recovery_quiesce_is_clear(
    const iot_recovery_quiesce_t *state)
{
    return state != NULL && state->initialized &&
           !state->operation_active &&
           !state->destructive_tail_active &&
           !state->quiesce_requested;
}