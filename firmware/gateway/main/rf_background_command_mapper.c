#include "rf_background_command_mapper.h"

#include <stddef.h>
#include <string.h>

_Static_assert(RF_BG_MAPPER_MODE_AUTO == RF_CQ_MODE_AUTO,
               "RF raw AUTO mode must match CQ AUTO mode");
_Static_assert(RF_BG_MAPPER_MODE_MANUAL == RF_CQ_MODE_MANUAL,
               "RF raw MANUAL mode must match CQ MANUAL mode");
_Static_assert(RF_BG_MAPPER_MODE_UNKNOWN == RF_BG_SET_MODE_UNKNOWN,
               "RF raw UNKNOWN mode must match CQ UNKNOWN mode");
_Static_assert(RF_BG_MAPPER_POWER_UNKNOWN_DBM ==
               RF_BG_SET_POWER_UNKNOWN_DBM,
               "RF raw UNKNOWN power must match CQ UNKNOWN power");

static bool known_node_mode(uint8_t mode)
{
    return mode == RF_BG_MAPPER_MODE_AUTO ||
           mode == RF_BG_MAPPER_MODE_MANUAL ||
           mode == RF_BG_MAPPER_MODE_UNKNOWN;
}

static bool known_desired_mode(uint8_t mode)
{
    return mode == RF_BG_MAPPER_MODE_AUTO ||
           mode == RF_BG_MAPPER_MODE_MANUAL;
}

static bool known_power(int8_t power_dbm)
{
    return power_dbm == RF_BG_MAPPER_POWER_UNKNOWN_DBM ||
           rf_cq_supported_power(power_dbm);
}

bool rf_background_command_mapper_map(
    const rf_background_command_mapper_input_t *input,
    rf_background_command_mapper_output_t *output)
{
    if (input == NULL || output == NULL) {
        return false;
    }

    memset(output, 0, sizeof(*output));
    output->result = RF_BG_MAPPER_INVALID_ARG;

    if (input->now_ms == 0U) {
        return true;
    }
    if (!known_power(input->current_power_dbm)) {
        output->result = RF_BG_MAPPER_INVALID_POWER;
        return true;
    }
    if (!known_node_mode(input->node_mode) ||
        !known_desired_mode(input->desired_mode)) {
        output->result = RF_BG_MAPPER_INVALID_MODE;
        return true;
    }
    if (!rf_cq_supported_power(input->desired_manual_power_dbm)) {
        output->result = RF_BG_MAPPER_INVALID_POWER;
        return true;
    }
    if (input->report_waiting == (input->wait_started_ms == 0U) ||
        (input->report_submitting && !input->report_waiting)) {
        output->result = RF_BG_MAPPER_INVALID_STATE;
        return true;
    }
    if (input->next_command_id == 0U ||
        input->policy_generation == 0U ||
        input->policy_generation == RF_CQ_GENERATION_ANY) {
        output->result = RF_BG_MAPPER_INVALID_STATE;
        return true;
    }
    if (input->background_command_active) {
        if (input->active_command_id == 0U ||
            input->active_command_generation == 0U ||
            input->active_command_generation == RF_CQ_GENERATION_ANY ||
            input->next_command_id == input->active_command_id) {
            output->result = RF_BG_MAPPER_INVALID_STATE;
            return true;
        }
    } else if (input->active_command_id != 0U ||
               input->active_command_generation != 0U) {
        output->result = RF_BG_MAPPER_INVALID_STATE;
        return true;
    }

    output->runtime.now_ms = input->now_ms;
    output->runtime.last_eval_ms = input->last_eval_ms;
    output->runtime.node_addr = input->node_addr;
    output->runtime.last_rx_ms = input->last_rx_ms;
    output->runtime.confirmed_ms = input->confirmed_ms;
    output->runtime.current_power_dbm = input->current_power_dbm;
    output->runtime.node_mode = input->node_mode;
    output->runtime.desired_mode = input->desired_mode;
    output->runtime.desired_manual_power_dbm =
        input->desired_manual_power_dbm;
    output->runtime.policy_apply_pending = input->policy_apply_pending;
    output->runtime.tx_backoff_until_ms = input->tx_backoff_until_ms;
    output->runtime.user_intent_active = input->user_intent_active;
    output->runtime.report_waiting = input->report_waiting;
    output->runtime.report_submitting = input->report_submitting;
    output->runtime.wait_started_ms = input->wait_started_ms;
    output->runtime.dispatch_active = input->dispatch_active;
    output->runtime.background_command_active =
        input->background_command_active;
    output->runtime.active_command_id = input->active_command_id;
    output->runtime.active_command_generation =
        input->active_command_generation;
    output->runtime.next_command_id = input->next_command_id;
    output->runtime.policy_generation = input->policy_generation;
    output->result = RF_BG_MAPPER_OK;
    return true;
}
