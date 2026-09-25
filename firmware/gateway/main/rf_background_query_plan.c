#include "rf_background_query_plan.h"

#include <stddef.h>

static bool supported_power(int8_t power_dbm)
{
    static const int8_t powers[] = {-10, 0, 8, 14, 18, 20};

    for (size_t i = 0; i < sizeof(powers) / sizeof(powers[0]); ++i) {
        if (powers[i] == power_dbm) {
            return true;
        }
    }
    return false;
}

bool rf_background_query_plan_create(
    const rf_background_query_plan_state_t *state,
    rf_background_query_plan_t *plan)
{
    uint32_t age;

    if (state == NULL || plan == NULL || state->now_ms == 0U) {
        return false;
    }

    plan->result = RF_BG_QUERY_PLAN_INVALID_ARG;

    if (state->node_addr == 0U ||
        (state->current_power_dbm != RF_BG_POWER_UNKNOWN_DBM &&
         !supported_power(state->current_power_dbm))) {
        return true;
    }

    // Match the mature legacy policy: a node that has not been seen recently
    // is reset first and not queried during the same evaluation.
    if (state->last_rx_ms == 0U) {
        plan->result = RF_BG_QUERY_PLAN_NODE_STALE;
        return true;
    }
    age = state->now_ms - state->last_rx_ms;
    if (age > RF_BG_NODE_FRESH_MS) {
        plan->result = RF_BG_QUERY_PLAN_NODE_STALE;
        return true;
    }

    if (state->wait_started_ms != 0U) {
        age = state->now_ms - state->wait_started_ms;
        if (age < RF_BG_WAIT_SETTLE_MS) {
            plan->result = RF_BG_QUERY_PLAN_NO_SEND_WAITING;
            return true;
        }
        plan->result = RF_BG_QUERY_PLAN_SEND;
        return true;
    }

    if (state->current_power_dbm == RF_BG_POWER_UNKNOWN_DBM) {
        plan->result = RF_BG_QUERY_PLAN_SEND;
        return true;
    }

    if (state->confirmed_ms != 0U) {
        age = state->now_ms - state->confirmed_ms;
        if (age >= RF_BG_RECONCILE_MS) {
            plan->result = RF_BG_QUERY_PLAN_SEND;
            return true;
        }
    }

    plan->result = RF_BG_QUERY_PLAN_NO_SEND;
    return true;
}
