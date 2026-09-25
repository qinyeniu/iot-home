#include "rf_background_execution_plan.h"

#include <string.h>

static bool node_is_stale(const rf_background_execution_state_t *state)
{
    uint32_t age;

    if (state->node_addr == 0U) {
        return false;
    }
    if (state->last_rx_ms == 0U) {
        return true;
    }
    age = state->now_ms - state->last_rx_ms;
    return age > RF_BG_NODE_FRESH_MS;
}

static bool waiting_is_due(const rf_background_execution_state_t *state,
                           bool node_stale)
{
    uint32_t age;

    if (node_stale || !state->report_waiting ||
        state->wait_started_ms == 0U) {
        return false;
    }
    age = state->now_ms - state->wait_started_ms;
    return age >= RF_BG_WAIT_SETTLE_MS;
}

static bool command_identity_valid(
    const rf_background_execution_state_t *state)
{
    return state->next_command_id != 0U &&
           state->policy_generation != 0U &&
           state->policy_generation != RF_CQ_GENERATION_ANY;
}

bool rf_background_execution_plan_create(
    const rf_background_execution_state_t *state,
    rf_background_execution_plan_t *plan)
{
    rf_background_admission_state_t admission;
    rf_background_admission_result_t admission_result =
        RF_BACKGROUND_ADMISSION_ADMIT;
    rf_background_query_plan_state_t query_state;
    rf_background_query_plan_t query_plan;
    rf_background_set_plan_state_t set_state;
    rf_background_set_plan_t set_plan;
    rf_cq_result_t build_result;
    bool node_stale;
    bool waiting_due;

    if (state == NULL || plan == NULL || state->now_ms == 0U) {
        return false;
    }

    memset(plan, 0, sizeof(*plan));
    plan->result = RF_BG_EXEC_INVALID_ARG;

    if ((state->report_waiting && state->wait_started_ms == 0U) ||
        (!state->report_waiting && state->wait_started_ms != 0U) ||
        (state->report_submitting && !state->report_waiting)) {
        return true;
    }

    node_stale = node_is_stale(state);
    waiting_due = waiting_is_due(state, node_stale);

    admission.now_ms = state->now_ms;
    admission.last_eval_ms = state->last_eval_ms;
    admission.node_addr = state->node_addr;
    admission.tx_backoff_active = state->tx_backoff_active;
    admission.user_intent_active = state->user_intent_active;
    admission.report_waiting =
        state->report_waiting && !node_stale && !waiting_due;
    admission.report_submitting = state->report_submitting;
    admission.dispatch_active = state->dispatch_active;
    admission.background_command_active =
        state->background_command_active && !node_stale && !waiting_due;

    if (!rf_background_admission_decide(&admission, &admission_result)) {
        return false;
    }
    if (admission_result != RF_BACKGROUND_ADMISSION_ADMIT) {
        plan->result = RF_BG_EXEC_REJECTED;
        plan->reject_reason = admission_result;
        return true;
    }

    if (node_stale) {
        plan->result = RF_BG_EXEC_NODE_STALE;
        plan->update_last_eval = true;
        plan->clear_stale_state = true;
        plan->clear_existing_waiting = state->report_waiting;
        plan->mark_policy_pending = true;
        return true;
    }

    if (waiting_due) {
        if (!command_identity_valid(state)) {
            return true;
        }
        build_result = rf_cq_build_periodic_query(
            state->next_command_id, state->policy_generation,
            state->now_ms, &plan->command);
        if (build_result != RF_CQ_OK) {
            return true;
        }
        plan->result = RF_BG_EXEC_READY_QUERY;
        plan->update_last_eval = true;
        plan->background_active = true;
        plan->clear_existing_waiting = true;
        return true;
    }

    query_state.now_ms = state->now_ms;
    query_state.node_addr = state->node_addr;
    query_state.last_rx_ms = state->last_rx_ms;
    query_state.confirmed_ms = state->confirmed_ms;
    query_state.wait_started_ms = state->wait_started_ms;
    query_state.current_power_dbm = state->current_power_dbm;

    if (!rf_background_query_plan_create(&query_state, &query_plan)) {
        return false;
    }

    if (query_plan.result == RF_BG_QUERY_PLAN_SEND) {
        if (!command_identity_valid(state)) {
            return true;
        }
        build_result = rf_cq_build_periodic_query(
            state->next_command_id, state->policy_generation,
            state->now_ms, &plan->command);
        if (build_result != RF_CQ_OK) {
            return true;
        }
        plan->result = RF_BG_EXEC_READY_QUERY;
        plan->update_last_eval = true;
        plan->background_active = true;
        return true;
    }

    if (query_plan.result != RF_BG_QUERY_PLAN_NO_SEND) {
        return true;
    }

    set_state.now_ms = state->now_ms;
    set_state.node_addr = state->node_addr;
    set_state.current_power_dbm = state->current_power_dbm;
    set_state.node_mode = state->node_mode;
    set_state.desired_mode = state->desired_mode;
    set_state.desired_manual_power_dbm = state->desired_manual_power_dbm;
    set_state.policy_apply_pending = state->policy_apply_pending;

    if (!rf_background_set_plan_create(&set_state, &set_plan)) {
        return false;
    }

    if (set_plan.result == RF_BG_SET_PLAN_SEND) {
        if (!command_identity_valid(state)) {
            return true;
        }
        build_result = rf_cq_build_periodic_set(
            state->next_command_id, set_plan.set_mode,
            set_plan.set_power_dbm, state->policy_generation,
            state->now_ms, &plan->command);
        if (build_result != RF_CQ_OK) {
            return true;
        }
        plan->result = RF_BG_EXEC_READY_SET;
        plan->update_last_eval = true;
        plan->background_active = true;
        plan->mark_policy_pending = set_plan.mark_policy_pending;
        return true;
    }

    if (set_plan.result == RF_BG_SET_PLAN_CLEAR_PENDING) {
        plan->result = RF_BG_EXEC_POLICY_ALREADY_APPLIED;
        plan->update_last_eval = true;
        plan->clear_policy_pending = true;
        return true;
    }

    if (set_plan.result == RF_BG_SET_PLAN_NO_SEND) {
        plan->result = RF_BG_EXEC_IDLE;
        plan->update_last_eval = true;
        return true;
    }

    return true;
}
