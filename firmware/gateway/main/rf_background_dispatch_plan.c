#include "rf_background_dispatch_plan.h"

#include <stddef.h>

#include "rf_dispatch_plan.h"

_Static_assert(RF_CQ_MODE_AUTO == RF_DISPATCH_MODE_AUTO,
               "CQ AUTO mode must match packet AUTO mode");
_Static_assert(RF_CQ_MODE_MANUAL == RF_DISPATCH_MODE_MANUAL,
               "CQ MANUAL mode must match packet MANUAL mode");
_Static_assert(RF_CQ_COMMAND_QUERY == RF_DISPATCH_COMMAND_QUERY,
               "CQ QUERY command must match packet QUERY command");
_Static_assert(RF_CQ_COMMAND_SET == RF_DISPATCH_COMMAND_SET,
               "CQ SET command must match packet SET command");

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint8_t next_packet_sequence(uint8_t current_sequence)
{
    if (current_sequence >= 0x7fu) {
        return 1u;
    }
    return (uint8_t)(current_sequence + 1u);
}

static bool background_command_valid(const rf_dispatch_cmd_t *command)
{
    return command->source == RF_CQ_SOURCE_PERIODIC &&
           rf_cq_validate(command) == RF_CQ_OK;
}

static void reset_plan(rf_background_dispatch_plan_t *plan)
{
    plan->status = RF_BG_DISPATCH_INVALID_ARG;
    plan->node_addr = 0U;
    plan->seq = 0U;
    plan->command = 0U;
    plan->packet_power_dbm = RF_BG_DISPATCH_POWER_UNKNOWN_DBM;
    plan->packet_mode = RF_BG_DISPATCH_MODE_UNKNOWN;
    plan->expected_report_power_dbm = RF_BG_DISPATCH_POWER_UNKNOWN_DBM;
    plan->expected_report_mode = RF_BG_DISPATCH_MODE_UNKNOWN;
}

static bool identity_matches(const rf_dispatch_cmd_t *command,
                             const rf_background_dispatch_state_t *state)
{
    return state->active_command_id == command->id &&
           state->active_command_generation == command->policy_generation;
}

static void set_send_query(rf_background_dispatch_plan_t *plan,
                           const rf_background_dispatch_state_t *state,
                           uint8_t seq)
{
    plan->status = RF_BG_DISPATCH_SEND;
    plan->node_addr = state->node_addr;
    plan->seq = seq;
    plan->command = RF_DISPATCH_COMMAND_QUERY;
    plan->packet_power_dbm = 0;
    plan->packet_mode = RF_DISPATCH_MODE_AUTO;
    plan->expected_report_power_dbm =
        RF_BG_DISPATCH_POWER_UNKNOWN_DBM;
    plan->expected_report_mode = RF_BG_DISPATCH_MODE_UNKNOWN;
}

static void set_send_set(rf_background_dispatch_plan_t *plan,
                         const rf_dispatch_cmd_t *command,
                         const rf_background_dispatch_state_t *state,
                         uint8_t seq)
{
    plan->status = RF_BG_DISPATCH_SEND;
    plan->node_addr = state->node_addr;
    plan->seq = seq;
    plan->command = RF_DISPATCH_COMMAND_SET;
    plan->packet_power_dbm = command->desired_power_dbm;
    plan->packet_mode = command->desired_mode;
    plan->expected_report_power_dbm = command->desired_power_dbm;
    plan->expected_report_mode = command->desired_mode;
}

bool rf_background_dispatch_plan_create(
    const rf_dispatch_cmd_t *command,
    const rf_background_dispatch_state_t *state,
    rf_background_dispatch_plan_t *plan)
{
    uint8_t seq;

    if (command == NULL || state == NULL || plan == NULL) {
        return false;
    }
    reset_plan(plan);

    if (!background_command_valid(command)) {
        return true;
    }
    if (!identity_matches(command, state)) {
        plan->status = RF_BG_DISPATCH_DROP_STALE;
        return true;
    }
    if (deadline_reached(state->now_ms, command->deadline_ms)) {
        plan->status = RF_BG_DISPATCH_CLOSE_EXPIRED;
        return true;
    }
    if (state->user_intent_active ||
        state->report_waiting ||
        state->report_submitting ||
        state->dispatch_active) {
        plan->status = RF_BG_DISPATCH_CLOSE_STATE_CHANGED;
        return true;
    }
    if (state->node_addr == 0U) {
        plan->status = RF_BG_DISPATCH_CLOSE_INACTIVE;
        return true;
    }

    seq = next_packet_sequence(state->packet_sequence);
    if (command->command == RF_CQ_COMMAND_QUERY) {
        set_send_query(plan, state, seq);
        return true;
    }
    if (command->command == RF_CQ_COMMAND_SET) {
        set_send_set(plan, command, state, seq);
        return true;
    }

    return true;
}
