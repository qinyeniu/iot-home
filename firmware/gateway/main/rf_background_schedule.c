#include "rf_background_schedule.h"

#include <stddef.h>

bool rf_background_admission_decide(
    const rf_background_admission_state_t *state,
    rf_background_admission_result_t *result)
{
    if (state == NULL || result == NULL || state->now_ms == 0U) {
        return false;
    }

    if (state->now_ms - state->last_eval_ms < RF_BACKGROUND_EVAL_MS) {
        *result = RF_BACKGROUND_REJECT_EVAL_COOLDOWN;
        return true;
    }
    if (state->tx_backoff_active) {
        *result = RF_BACKGROUND_REJECT_TX_BACKOFF;
        return true;
    }
    if (state->node_addr == 0U) {
        *result = RF_BACKGROUND_REJECT_NODE_INACTIVE;
        return true;
    }
    if (state->user_intent_active) {
        *result = RF_BACKGROUND_REJECT_USER_PRIORITY;
        return true;
    }
    if (state->report_waiting || state->report_submitting) {
        *result = RF_BACKGROUND_REJECT_REPORT_WAITING;
        return true;
    }
    if (state->dispatch_active) {
        *result = RF_BACKGROUND_REJECT_DISPATCH_ACTIVE;
        return true;
    }
    if (state->background_command_active) {
        *result = RF_BACKGROUND_REJECT_BACKGROUND_ACTIVE;
        return true;
    }

    *result = RF_BACKGROUND_ADMISSION_ADMIT;
    return true;
}
