#include "rf_background_runtime_adapter.h"

#include <string.h>

static bool backoff_active(const rf_background_runtime_state_t *state)
{
    return state->tx_backoff_until_ms != 0U &&
           (int32_t)(state->now_ms - state->tx_backoff_until_ms) < 0;
}

static bool runtime_values_consistent(
    const rf_background_runtime_state_t *state)
{
    bool node_power_valid;
    bool node_mode_valid;

    if (state->next_command_id == 0U ||
        state->policy_generation == 0U ||
        state->policy_generation == RF_CQ_GENERATION_ANY ||
        (state->desired_mode != RF_CQ_MODE_AUTO &&
         state->desired_mode != RF_CQ_MODE_MANUAL) ||
        (state->desired_mode == RF_CQ_MODE_MANUAL &&
         !rf_cq_supported_power(state->desired_manual_power_dbm)) ||
        (state->report_waiting == (state->wait_started_ms == 0U)) ||
        (state->report_submitting &&
         (!state->report_waiting || state->wait_started_ms == 0U))) {
        return false;
    }

    node_power_valid =
        state->current_power_dbm == RF_BG_SET_POWER_UNKNOWN_DBM ||
        rf_cq_supported_power(state->current_power_dbm);
    node_mode_valid =
        state->node_mode == RF_BG_SET_MODE_UNKNOWN ||
        state->node_mode == RF_CQ_MODE_AUTO ||
        state->node_mode == RF_CQ_MODE_MANUAL;
    if (!node_power_valid || !node_mode_valid) {
        return false;
    }

    if (state->background_command_active) {
        if (state->active_command_id == 0U ||
            state->active_command_generation == 0U ||
            state->active_command_generation == RF_CQ_GENERATION_ANY) {
            return false;
        }
    } else if (state->active_command_id != 0U ||
               state->active_command_generation != 0U) {
        return false;
    }

    return true;
}

static void clear_waiting(rf_background_runtime_state_t *state)
{
    state->report_waiting = false;
    state->report_submitting = false;
    state->wait_started_ms = 0U;
}

static void clear_background_ownership(rf_background_runtime_state_t *state)
{
    state->background_command_active = false;
    state->active_command_id = 0U;
    state->active_command_generation = 0U;
}

bool rf_background_runtime_adapter_apply(
    const rf_background_runtime_state_t *state,
    rf_background_runtime_adapter_result_t *result)
{
    rf_background_execution_state_t execution;
    rf_background_execution_plan_t plan;

    if (state == NULL || result == NULL || state->now_ms == 0U) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->result = RF_BG_EXEC_INVALID_ARG;
    result->next_state = *state;

    if (!runtime_values_consistent(state)) {
        return true;
    }

    memset(&execution, 0, sizeof(execution));
    execution.now_ms = state->now_ms;
    execution.last_eval_ms = state->last_eval_ms;
    execution.node_addr = state->node_addr;
    execution.last_rx_ms = state->last_rx_ms;
    execution.confirmed_ms = state->confirmed_ms;
    execution.current_power_dbm = state->current_power_dbm;
    execution.node_mode = state->node_mode;
    execution.desired_mode = state->desired_mode;
    execution.desired_manual_power_dbm = state->desired_manual_power_dbm;
    execution.policy_apply_pending = state->policy_apply_pending;
    execution.tx_backoff_active = backoff_active(state);
    execution.user_intent_active = state->user_intent_active;
    execution.report_waiting = state->report_waiting;
    execution.report_submitting = state->report_submitting;
    execution.wait_started_ms = state->wait_started_ms;
    execution.dispatch_active = state->dispatch_active;
    execution.background_command_active = state->background_command_active;
    execution.next_command_id = state->next_command_id;
    execution.policy_generation = state->policy_generation;

    if (!rf_background_execution_plan_create(&execution, &plan)) {
        return false;
    }

    result->result = plan.result;
    result->reject_reason = plan.reject_reason;

    if (plan.result == RF_BG_EXEC_REJECTED ||
        plan.result == RF_BG_EXEC_INVALID_ARG) {
        return true;
    }

    result->state_changed = true;

    if (plan.update_last_eval) {
        result->next_state.last_eval_ms = state->now_ms;
    }
    if (plan.clear_stale_state) {
        result->next_state.current_power_dbm = RF_BG_SET_POWER_UNKNOWN_DBM;
        result->next_state.node_mode = RF_BG_SET_MODE_UNKNOWN;
        result->next_state.confirmed_ms = 0U;
        clear_background_ownership(&result->next_state);
    }
    if (plan.clear_existing_waiting) {
        clear_waiting(&result->next_state);
    }
    if (plan.mark_policy_pending) {
        result->next_state.policy_apply_pending = true;
    }
    if (plan.background_active) {
        result->next_state.background_command_active = true;
        result->next_state.active_command_id = plan.command.id;
        result->next_state.active_command_generation =
            plan.command.policy_generation;
        result->command = plan.command;
        result->has_command = true;
    }
    if (plan.clear_policy_pending) {
        result->next_state.policy_apply_pending = false;
    }

    return true;
}

bool rf_background_runtime_rollback_command(
    const rf_background_runtime_state_t *state,
    const rf_dispatch_cmd_t *command,
    rf_background_runtime_rollback_result_t *result,
    rf_background_runtime_state_t *next_state)
{
    if (state == NULL || command == NULL || result == NULL ||
        next_state == NULL || state->now_ms == 0U) {
        return false;
    }

    *result = RF_BG_RUNTIME_ROLLBACK_INVALID_ARG;
    *next_state = *state;

    if (command->id == 0U ||
        command->policy_generation == 0U ||
        command->policy_generation == RF_CQ_GENERATION_ANY) {
        return true;
    }
    if (command->source != RF_CQ_SOURCE_PERIODIC) {
        return true;
    }
    if (!state->background_command_active ||
        state->active_command_id != command->id ||
        state->active_command_generation != command->policy_generation ||
        state->policy_generation != command->policy_generation) {
        *result = RF_BG_RUNTIME_ROLLBACK_NOT_MATCHED;
        return true;
    }
    if (state->user_intent_active ||
        state->report_waiting ||
        state->report_submitting ||
        state->dispatch_active) {
        *result = RF_BG_RUNTIME_ROLLBACK_NOT_MATCHED;
        return true;
    }

    clear_background_ownership(next_state);
    *result = RF_BG_RUNTIME_ROLLBACK_DONE;
    return true;
}
