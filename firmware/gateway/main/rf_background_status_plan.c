#include "rf_background_status_plan.h"

#include <stddef.h>
#include <string.h>

#include "rf_dispatch_plan.h"

static bool valid_mode(uint8_t mode)
{
    return mode == RF_CQ_MODE_AUTO || mode == RF_CQ_MODE_MANUAL ||
           mode == RF_DISPATCH_MODE_UNKNOWN;
}

static bool valid_report(uint8_t mode, int8_t power_dbm)
{
    if (mode == RF_DISPATCH_MODE_UNKNOWN) {
        return power_dbm == RF_DISPATCH_POWER_UNKNOWN_DBM;
    }
    return rf_cq_supported_power(power_dbm);
}

static bool active_command_valid(const rf_dispatch_cmd_t *command)
{
    return command->source == RF_CQ_SOURCE_PERIODIC &&
           command->id != 0U &&
           command->policy_generation != 0U &&
           command->policy_generation != RF_CQ_GENERATION_ANY &&
           (command->command == RF_CQ_COMMAND_QUERY ||
            command->command == RF_CQ_COMMAND_SET);
}

static void set_negative_close(rf_background_status_plan_t *plan,
                               rf_background_publish_status_t publish,
                               rf_background_command_table_action_t table,
                               bool notice_allowed, bool set_pending,
                               bool pending_value)
{
    plan->clear_background = true;
    plan->set_policy_pending = set_pending;
    plan->policy_pending_value = pending_value;
    plan->table_action = table;
    plan->publish_status = publish;

    if (notice_allowed) {
        plan->result = RF_BG_STATUS_PLAN_PUBLISH;
    } else {
        plan->result = RF_BG_STATUS_PLAN_INTERNAL_CLOSE;
    }
}

static bool handle_query_report(
    const rf_background_status_state_t *state,
    rf_background_status_plan_t *plan)
{
    if (!state->matched_transaction ||
        state->reported_mode == RF_DISPATCH_MODE_UNKNOWN ||
        state->reported_power_dbm == RF_DISPATCH_POWER_UNKNOWN_DBM) {
        plan->result = RF_BG_STATUS_PLAN_NO_ACTION;
        return true;
    }

    plan->result = RF_BG_STATUS_PLAN_PUBLISH;
    plan->publish_status = RF_BG_STATUS_PUBLISH_REPORT_RECEIVED;
    plan->table_action = RF_BG_COMMAND_TABLE_NONE;
    plan->reported_mode = state->reported_mode;
    plan->reported_power_dbm = state->reported_power_dbm;
    plan->clear_background = true;
    return true;
}

static bool handle_set_report(
    const rf_background_status_state_t *state,
    rf_background_status_plan_t *plan)
{
    rf_dispatch_after_report_state_t after;
    rf_dispatch_after_report_result_t after_result =
        RF_DISPATCH_AFTER_REPORT_NONE;

    memset(&after, 0, sizeof(after));
    after.intent_command = RF_DISPATCH_COMMAND_SET;
    after.desired_mode = state->command.desired_mode;
    after.target_power_dbm = state->command.desired_power_dbm;
    after.policy_apply_pending = state->policy_apply_pending;
    after.reported_power_dbm = state->reported_power_dbm;
    after.reported_mode = state->reported_mode;
    after.matched_transaction = state->matched_transaction;
    after.node_initiated_report = state->node_initiated_report;

    if (!rf_dispatch_after_report_create(&after, &after_result)) {
        return false;
    }

    if (after_result == RF_DISPATCH_AFTER_REPORT_COMPLETE) {
        plan->result = RF_BG_STATUS_PLAN_PUBLISH;
        plan->publish_status = RF_BG_STATUS_PUBLISH_CONFIRMED;
        plan->table_action = RF_BG_COMMAND_TABLE_ACKNOWLEDGED;
        plan->reported_mode = state->reported_mode;
        plan->reported_power_dbm = state->reported_power_dbm;
        plan->clear_background = true;
        plan->set_policy_pending = true;
        plan->policy_pending_value = false;
        return true;
    }

    // RETRY is owned by the existing dispatch/retry path; a mismatched report
    // is not turned into a terminal failure.
    plan->result = RF_BG_STATUS_PLAN_NO_ACTION;
    return true;
}

bool rf_background_status_plan_create(
    const rf_background_status_state_t *state,
    rf_background_status_plan_t *plan)
{
    if (state == NULL || plan == NULL || state->now_ms == 0U) {
        return false;
    }

    *plan = (rf_background_status_plan_t){
        .result = RF_BG_STATUS_PLAN_INVALID_ARG,
        .reported_power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM,
        .reported_mode = RF_DISPATCH_MODE_UNKNOWN,
    };

    if (!active_command_valid(&state->command) ||
        rf_cq_validate(&state->command) != RF_CQ_OK ||
        state->event > RF_BG_STATUS_EVENT_SEND_FAILED ||
        !valid_mode(state->reported_mode) ||
        !valid_report(state->reported_mode, state->reported_power_dbm)) {
        return true;
    }
    if (state->event == RF_BG_STATUS_EVENT_MATCHED_REPORT &&
        (!state->matched_transaction && !state->node_initiated_report)) {
        return true;
    }
    if (state->event != RF_BG_STATUS_EVENT_MATCHED_REPORT &&
        (state->matched_transaction || state->node_initiated_report)) {
        return true;
    }

    if (state->event == RF_BG_STATUS_EVENT_MATCHED_REPORT) {
        if (state->command.command == RF_CQ_COMMAND_QUERY) {
            return handle_query_report(state, plan);
        }
        return handle_set_report(state, plan);
    }

    plan->reported_power_dbm = state->reported_power_dbm;
    plan->reported_mode = state->reported_mode;

    if (state->event == RF_BG_STATUS_EVENT_WAIT_TIMEOUT) {
        bool set_pending = state->command.command == RF_CQ_COMMAND_SET;
        set_negative_close(plan, RF_BG_STATUS_PUBLISH_TIMEOUT,
                           RF_BG_COMMAND_TABLE_TIMEOUT,
                           state->notice_allowed, set_pending, true);
        return true;
    }

    {
        bool set_pending = state->command.command == RF_CQ_COMMAND_SET;
        set_negative_close(plan, RF_BG_STATUS_PUBLISH_FAILED,
                           RF_BG_COMMAND_TABLE_FAILED,
                           state->notice_allowed, set_pending, true);
    }
    return true;
}

