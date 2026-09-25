// CQ-2b: pure RF dispatch planning policy.

#include "rf_dispatch_plan.h"

static bool supported_power(int8_t power_dbm)
{
    return rf_cq_supported_power(power_dbm);
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint8_t next_packet_sequence(uint8_t current_sequence)
{
    uint16_t candidate = (uint16_t)((uint16_t)current_sequence + 1u);

    if (candidate == 0u || candidate >= 0x80u) {
        candidate = 1u;
    }
    return (uint8_t)candidate;
}

static void reset_plan(rf_dispatch_plan_t *plan)
{
    plan->status = RF_DISPATCH_PLAN_ERR_INVALID_ARG;
    plan->node_addr = 0U;
    plan->seq = 0U;
    plan->command = 0U;
    plan->power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM;
    plan->mode = RF_DISPATCH_MODE_UNKNOWN;
    plan->publish_querying = false;
    plan->clear_waiting = false;
    plan->expected_report_power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM;
    plan->expected_report_mode = RF_DISPATCH_MODE_UNKNOWN;
}

static void set_no_send(rf_dispatch_plan_t *plan, uint16_t node_addr,
                        bool clear_waiting)
{
    plan->status = RF_DISPATCH_PLAN_NO_SEND_POLICY_SAVED;
    plan->node_addr = node_addr;
    plan->clear_waiting = clear_waiting;
}

static void set_send_query(rf_dispatch_plan_t *plan, uint16_t node_addr,
                           uint8_t seq, bool publish_querying)
{
    plan->status = RF_DISPATCH_PLAN_SEND;
    plan->node_addr = node_addr;
    plan->seq = seq;
    plan->command = RF_DISPATCH_COMMAND_QUERY;
    plan->power_dbm = 0;
    plan->mode = RF_DISPATCH_MODE_AUTO;
    plan->publish_querying = publish_querying;
    plan->expected_report_power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM;
    plan->expected_report_mode = RF_DISPATCH_MODE_UNKNOWN;
}

static void set_send_set(rf_dispatch_plan_t *plan, uint16_t node_addr,
                         uint8_t seq, int8_t power_dbm, uint8_t mode)
{
    plan->status = RF_DISPATCH_PLAN_SEND;
    plan->node_addr = node_addr;
    plan->seq = seq;
    plan->command = RF_DISPATCH_COMMAND_SET;
    plan->power_dbm = power_dbm;
    plan->mode = mode;
    plan->publish_querying = false;
    plan->expected_report_power_dbm = power_dbm;
    plan->expected_report_mode = mode;
}

bool rf_dispatch_plan_create(const rf_dispatch_cmd_t *cmd,
                             const rf_dispatch_plan_state_t *state,
                             rf_dispatch_plan_t *plan)
{
    uint8_t seq;

    if (cmd == NULL || state == NULL || plan == NULL) {
        return false;
    }
    reset_plan(plan);

    if (cmd->source != RF_CQ_SOURCE_USER ||
        rf_cq_validate(cmd) != RF_CQ_OK ||
        (state->node_power_dbm != RF_DISPATCH_POWER_UNKNOWN_DBM &&
         !supported_power(state->node_power_dbm))) {
        return true;
    }
    if (deadline_reached(state->now_ms, cmd->deadline_ms)) {
        plan->status = RF_DISPATCH_PLAN_ERR_EXPIRED;
        return true;
    }

    seq = next_packet_sequence(state->packet_sequence);

    if (cmd->command == RF_CQ_COMMAND_QUERY) {
        if (state->report_waiting &&
            !deadline_reached(state->now_ms, state->wait_deadline_ms)) {
            plan->status = RF_DISPATCH_PLAN_ERR_WAITING;
            return true;
        }
        if (state->node_addr == 0U) {
            plan->status = RF_DISPATCH_PLAN_ERR_INACTIVE;
            return true;
        }
        set_send_query(plan, state->node_addr, seq, true);
        return true;
    }

    if (cmd->command != RF_CQ_COMMAND_SET) {
        return true;
    }

    /* MANUAL/AUTO supersedes an old waiting transaction. */
    plan->clear_waiting = true;

    if (state->node_addr == 0U) {
        set_no_send(plan, 0U, true);
        return true;
    }

    if (cmd->desired_mode == RF_CQ_MODE_MANUAL) {
        if (state->node_power_dbm == RF_DISPATCH_POWER_UNKNOWN_DBM) {
            set_send_query(plan, state->node_addr, seq, false);
            plan->clear_waiting = true;
            return true;
        }
        set_send_set(plan, state->node_addr, seq, cmd->desired_power_dbm,
                     RF_DISPATCH_MODE_MANUAL);
        plan->clear_waiting = true;
        return true;
    }

    if (cmd->desired_mode != RF_CQ_MODE_AUTO) {
        return true;
    }
    if (state->node_power_dbm == RF_DISPATCH_POWER_UNKNOWN_DBM) {
        set_no_send(plan, state->node_addr, true);
        return true;
    }

    /* AUTO must send the node's current power, never a queued placeholder. */
    set_send_set(plan, state->node_addr, seq, state->node_power_dbm,
                 RF_DISPATCH_MODE_AUTO);
    plan->clear_waiting = true;
    return true;
}