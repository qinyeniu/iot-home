#include "rf_background_set_plan.h"

#include <stddef.h>

bool rf_background_set_plan_create(
    const rf_background_set_plan_state_t *state,
    rf_background_set_plan_t *plan)
{
    if (state == NULL || plan == NULL || state->now_ms == 0U) {
        return false;
    }

    plan->result = RF_BG_SET_PLAN_INVALID_ARG;
    plan->set_power_dbm = 0;
    plan->set_mode = RF_CQ_MODE_AUTO;
    plan->mark_policy_pending = false;

    if (state->node_addr == 0U ||
        !rf_cq_supported_power(state->current_power_dbm) ||
        (state->desired_mode != RF_CQ_MODE_AUTO &&
         state->desired_mode != RF_CQ_MODE_MANUAL) ||
        (state->desired_mode == RF_CQ_MODE_MANUAL &&
         !rf_cq_supported_power(state->desired_manual_power_dbm)) ||
        (state->node_mode != RF_CQ_MODE_AUTO &&
         state->node_mode != RF_CQ_MODE_MANUAL &&
         state->node_mode != RF_BG_SET_MODE_UNKNOWN)) {
        return true;
    }

    if (state->desired_mode == RF_CQ_MODE_MANUAL) {
        if (state->node_mode == RF_CQ_MODE_MANUAL &&
            state->current_power_dbm == state->desired_manual_power_dbm) {
            plan->result = state->policy_apply_pending
                ? RF_BG_SET_PLAN_CLEAR_PENDING
                : RF_BG_SET_PLAN_NO_SEND;
            return true;
        }

        // Unknown mode is left to QUERY; if mode is known and still differs,
        // converge it with the requested MANUAL target.
        if (state->node_mode == RF_BG_SET_MODE_UNKNOWN) {
            plan->result = RF_BG_SET_PLAN_NO_SEND;
            return true;
        }

        plan->result = RF_BG_SET_PLAN_SEND;
        plan->set_power_dbm = state->desired_manual_power_dbm;
        plan->set_mode = RF_CQ_MODE_MANUAL;
        plan->mark_policy_pending = !state->policy_apply_pending;
        return true;
    }

    // Desired AUTO.
    if (state->node_mode == RF_CQ_MODE_AUTO) {
        plan->result = state->policy_apply_pending
            ? RF_BG_SET_PLAN_CLEAR_PENDING
            : RF_BG_SET_PLAN_NO_SEND;
        return true;
    }
    if (state->node_mode == RF_BG_SET_MODE_UNKNOWN) {
        plan->result = RF_BG_SET_PLAN_NO_SEND;
        return true;
    }

    plan->result = RF_BG_SET_PLAN_SEND;
    plan->set_power_dbm = state->current_power_dbm;
    plan->set_mode = RF_CQ_MODE_AUTO;
    plan->mark_policy_pending = !state->policy_apply_pending;
    return true;
}
