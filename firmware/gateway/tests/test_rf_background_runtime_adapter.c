#include "rf_background_runtime_adapter.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("CHECK failed: %s (%s:%d)", #condition, __FILE__, __LINE__); \
        ++s_failures; \
    } \
} while (0)

static rf_background_runtime_state_t base_state(void)
{
    rf_background_runtime_state_t state;

    memset(&state, 0, sizeof(state));
    state.now_ms = 400000U;
    state.last_eval_ms = 300000U;
    state.node_addr = 0x82cbU;
    state.last_rx_ms = 399000U;
    state.confirmed_ms = 399000U;
    state.current_power_dbm = -10;
    state.node_mode = RF_CQ_MODE_AUTO;
    state.desired_mode = RF_CQ_MODE_AUTO;
    state.desired_manual_power_dbm = 0;
    state.policy_apply_pending = false;
    state.next_command_id = 42U;
    state.policy_generation = 7U;
    return state;
}

static void test_invalid_arguments(void)
{
    rf_background_runtime_state_t state = base_state();
    rf_background_runtime_adapter_result_t result;

    CHECK(!rf_background_runtime_adapter_apply(NULL, &result));
    CHECK(!rf_background_runtime_adapter_apply(&state, NULL));
    state.now_ms = 0U;
    CHECK(!rf_background_runtime_adapter_apply(&state, &result));

    state = base_state();
    state.background_command_active = true;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);
    CHECK(!result.state_changed);

    state = base_state();
    state.active_command_id = 9U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.report_waiting = true;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.current_power_dbm = 1;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.desired_mode = 2U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.policy_generation = 0U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state.policy_generation = RF_CQ_GENERATION_ANY;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.desired_mode = RF_CQ_MODE_MANUAL;
    state.desired_manual_power_dbm = 1;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.node_mode = 2U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.background_command_active = true;
    state.active_command_id = 100U;
    state.active_command_generation = 0U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);

    state.active_command_generation = RF_CQ_GENERATION_ANY;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_INVALID_ARG);
}

static void test_rejected_unchanged(void)
{
    rf_background_runtime_state_t state = base_state();
    rf_background_runtime_adapter_result_t result;

    state.last_eval_ms = 399999U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_REJECTED);
    CHECK(result.reject_reason == RF_BACKGROUND_REJECT_EVAL_COOLDOWN);
    CHECK(!result.state_changed);
    CHECK(!result.has_command);
    CHECK(memcmp(&result.next_state, &state, sizeof(state)) == 0);

    state = base_state();
    state.tx_backoff_until_ms = 400001U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_REJECTED);
    CHECK(result.reject_reason == RF_BACKGROUND_REJECT_TX_BACKOFF);
    CHECK(memcmp(&result.next_state, &state, sizeof(state)) == 0);

    state.tx_backoff_until_ms = 400000U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_IDLE);

    state.tx_backoff_until_ms = 399999U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_IDLE);
}

static void test_idle_and_clear(void)
{
    rf_background_runtime_state_t state = base_state();
    rf_background_runtime_adapter_result_t result;

    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_IDLE);
    CHECK(result.state_changed);
    CHECK(result.next_state.last_eval_ms == 400000U);
    CHECK(!result.has_command);

    state = base_state();
    state.policy_apply_pending = true;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_POLICY_ALREADY_APPLIED);
    CHECK(!result.next_state.policy_apply_pending);
    CHECK(!result.next_state.background_command_active);
}

static void test_stale_state(void)
{
    rf_background_runtime_state_t state = base_state();
    rf_background_runtime_adapter_result_t result;

    state.last_rx_ms = 0U;
    state.confirmed_ms = 123U;
    state.current_power_dbm = 20;
    state.node_mode = RF_CQ_MODE_MANUAL;
    state.background_command_active = true;
    state.active_command_id = 100U;
    state.active_command_generation = 7U;
    state.report_waiting = true;
    state.wait_started_ms = 310000U;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_NODE_STALE);
    CHECK(result.state_changed);
    CHECK(!result.has_command);
    CHECK(result.next_state.last_eval_ms == 400000U);
    CHECK(result.next_state.node_addr == 0x82cbU);
    CHECK(result.next_state.desired_mode == RF_CQ_MODE_AUTO);
    CHECK(result.next_state.current_power_dbm == RF_BG_SET_POWER_UNKNOWN_DBM);
    CHECK(result.next_state.node_mode == RF_BG_SET_MODE_UNKNOWN);
    CHECK(result.next_state.confirmed_ms == 0U);
    CHECK(result.next_state.policy_apply_pending);
    CHECK(!result.next_state.background_command_active);
    CHECK(result.next_state.active_command_id == 0U);
    CHECK(!result.next_state.report_waiting);
    CHECK(!result.next_state.report_submitting);
    CHECK(result.next_state.wait_started_ms == 0U);
}

static void test_ready_query_and_set(void)
{
    rf_background_runtime_state_t state = base_state();
    rf_background_runtime_adapter_result_t result;

    state.current_power_dbm = RF_BG_SET_POWER_UNKNOWN_DBM;
    state.node_mode = RF_BG_SET_MODE_UNKNOWN;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_READY_QUERY);
    CHECK(result.has_command);
    CHECK(result.command.id == 42U);
    CHECK(result.command.command == RF_CQ_COMMAND_QUERY);
    CHECK(result.next_state.background_command_active);
    CHECK(result.next_state.active_command_id == 42U);
    CHECK(result.next_state.active_command_generation == 7U);
    CHECK(!result.next_state.policy_apply_pending);

    state.policy_apply_pending = true;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.next_state.policy_apply_pending);

    state = base_state();
    state.desired_mode = RF_CQ_MODE_MANUAL;
    state.desired_manual_power_dbm = 0;
    state.node_mode = RF_CQ_MODE_MANUAL;
    state.current_power_dbm = -10;
    CHECK(rf_background_runtime_adapter_apply(&state, &result));
    CHECK(result.result == RF_BG_EXEC_READY_SET);
    CHECK(result.command.command == RF_CQ_COMMAND_SET);
    CHECK(result.command.desired_mode == RF_CQ_MODE_MANUAL);
    CHECK(result.command.desired_power_dbm == 0);
    CHECK(result.next_state.active_command_id == 42U);
    CHECK(result.next_state.active_command_generation == 7U);
}

static void test_rollback(void)
{
    rf_background_runtime_state_t state = base_state();
    rf_background_runtime_state_t next;
    rf_background_runtime_rollback_result_t rollback;
    rf_dispatch_cmd_t command;

    state.background_command_active = true;
    state.active_command_id = 42U;
    state.active_command_generation = 7U;
    state.policy_apply_pending = true;
    memset(&command, 0, sizeof(command));
    command.id = 42U;
    command.source = RF_CQ_SOURCE_PERIODIC;
    command.policy_generation = 7U;

    CHECK(rf_background_runtime_rollback_command(
        &state, &command, &rollback, &next));
    CHECK(rollback == RF_BG_RUNTIME_ROLLBACK_DONE);
    CHECK(!next.background_command_active);
    CHECK(next.active_command_id == 0U);
    CHECK(next.active_command_generation == 0U);
    CHECK(next.policy_apply_pending);

    command.id = 43U;
    CHECK(rf_background_runtime_rollback_command(
        &state, &command, &rollback, &next));
    CHECK(rollback == RF_BG_RUNTIME_ROLLBACK_NOT_MATCHED);
    CHECK(next.background_command_active);

    command.id = 42U;
    command.source = RF_CQ_SOURCE_USER;
    CHECK(rf_background_runtime_rollback_command(
        &state, &command, &rollback, &next));
    CHECK(rollback == RF_BG_RUNTIME_ROLLBACK_INVALID_ARG);
    command.source = RF_CQ_SOURCE_PERIODIC;

    state.user_intent_active = true;
    CHECK(rf_background_runtime_rollback_command(
        &state, &command, &rollback, &next));
    CHECK(rollback == RF_BG_RUNTIME_ROLLBACK_NOT_MATCHED);
    state.user_intent_active = false;

    state.report_waiting = true;
    state.wait_started_ms = 300000U;
    CHECK(rf_background_runtime_rollback_command(
        &state, &command, &rollback, &next));
    CHECK(rollback == RF_BG_RUNTIME_ROLLBACK_NOT_MATCHED);
    state.report_waiting = false;
    state.wait_started_ms = 0U;

    state.dispatch_active = true;
    CHECK(rf_background_runtime_rollback_command(
        &state, &command, &rollback, &next));
    CHECK(rollback == RF_BG_RUNTIME_ROLLBACK_NOT_MATCHED);
    state.dispatch_active = false;

    state.policy_generation = 8U;
    CHECK(rf_background_runtime_rollback_command(
        &state, &command, &rollback, &next));
    CHECK(rollback == RF_BG_RUNTIME_ROLLBACK_NOT_MATCHED);

    state.now_ms = 0U;
    CHECK(!rf_background_runtime_rollback_command(
        &state, &command, &rollback, &next));
}

int main(void)
{
    test_invalid_arguments();
    test_rejected_unchanged();
    test_idle_and_clear();
    test_stale_state();
    test_ready_query_and_set();
    test_rollback();

    if (s_failures != 0) {
        printf("rf_background_runtime_adapter host tests: FAIL (%d)", s_failures);
        return 1;
    }
    puts("rf_background_runtime_adapter host tests: PASS");
    return 0;
}
