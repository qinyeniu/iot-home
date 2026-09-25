#include "rf_background_execution_plan.h"

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

static rf_background_execution_state_t base_state(void)
{
    rf_background_execution_state_t state;

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
    state.next_command_id = 42U;
    state.policy_generation = 7U;
    return state;
}

static void test_invalid_arguments(void)
{
    rf_background_execution_state_t state = base_state();
    rf_background_execution_plan_t plan;

    CHECK(!rf_background_execution_plan_create(NULL, &plan));
    CHECK(!rf_background_execution_plan_create(&state, NULL));
    state.now_ms = 0U;
    CHECK(!rf_background_execution_plan_create(&state, &plan));

    state = base_state();
    state.report_waiting = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.wait_started_ms = 100U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.report_submitting = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);

    state = base_state();
    state.node_mode = 2U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);
}

static void test_admission_rejections(void)
{
    rf_background_execution_plan_t plan;
    rf_background_execution_state_t state;

    state = base_state();
    state.last_eval_ms = 399999U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_REJECTED);
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_EVAL_COOLDOWN);
    CHECK(!plan.update_last_eval);
    CHECK(plan.command.id == 0U);

    state = base_state();
    state.tx_backoff_active = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_TX_BACKOFF);

    state = base_state();
    state.node_addr = 0U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_NODE_INACTIVE);

    state = base_state();
    state.user_intent_active = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_USER_PRIORITY);

    state = base_state();
    state.report_waiting = true;
    state.wait_started_ms = 390000U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_REPORT_WAITING);
    CHECK(!plan.clear_existing_waiting);

    state = base_state();
    state.report_waiting = true;
    state.report_submitting = true;
    state.wait_started_ms = 390000U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_REPORT_WAITING);

    state = base_state();
    state.dispatch_active = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_DISPATCH_ACTIVE);

    state = base_state();
    state.background_command_active = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_BACKGROUND_ACTIVE);
}

static void test_idle(void)
{
    rf_background_execution_state_t state = base_state();
    rf_background_execution_plan_t plan;

    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_IDLE);
    CHECK(plan.update_last_eval);
    CHECK(!plan.background_active);
    CHECK(!plan.clear_policy_pending);
    CHECK(plan.command.id == 0U);
}

static void test_stale_node(void)
{
    rf_background_execution_state_t state = base_state();
    rf_background_execution_plan_t plan;

    state.last_rx_ms = 0U;
    state.background_command_active = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_NODE_STALE);
    CHECK(plan.update_last_eval);
    CHECK(plan.clear_stale_state);
    CHECK(plan.mark_policy_pending);
    CHECK(!plan.background_active);
    CHECK(plan.command.id == 0U);

    state = base_state();
    state.last_rx_ms = 279999U;
    state.report_waiting = true;
    state.wait_started_ms = 399000U;
    state.policy_apply_pending = false;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_NODE_STALE);
    CHECK(plan.clear_existing_waiting);
    CHECK(plan.mark_policy_pending);

    // Stale wins over a due waiting transaction; no packet is produced.
    state = base_state();
    state.last_rx_ms = 0U;
    state.report_waiting = true;
    state.wait_started_ms = 310000U;
    state.background_command_active = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_NODE_STALE);
    CHECK(plan.clear_existing_waiting);
    CHECK(plan.command.id == 0U);

    state.last_rx_ms = 280000U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result != RF_BG_EXEC_NODE_STALE);
}

static void test_waiting_due_query(void)
{
    rf_background_execution_state_t state = base_state();
    rf_background_execution_plan_t plan;

    state.report_waiting = true;
    state.wait_started_ms = 310000U;
    state.background_command_active = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_READY_QUERY);
    CHECK(plan.update_last_eval);
    CHECK(plan.background_active);
    CHECK(plan.clear_existing_waiting);
    CHECK(plan.command.id == 42U);
    CHECK(plan.command.source == RF_CQ_SOURCE_PERIODIC);
    CHECK(plan.command.command == RF_CQ_COMMAND_QUERY);
    CHECK(plan.command.policy_generation == 7U);
    CHECK(plan.command.deadline_ms == 402000U);

    state.wait_started_ms = 310001U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_REJECTED);
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_REPORT_WAITING);

    state = base_state();
    state.report_waiting = true;
    state.wait_started_ms = 310000U;
    state.tx_backoff_active = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_REJECTED);
    CHECK(!plan.clear_existing_waiting);

    // An in-flight submission cannot be displaced merely because the wait
    // deadline has been reached.
    state = base_state();
    state.report_waiting = true;
    state.report_submitting = true;
    state.wait_started_ms = 310000U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_REJECTED);
    CHECK(plan.reject_reason == RF_BACKGROUND_REJECT_REPORT_WAITING);
    CHECK(!plan.update_last_eval);
    CHECK(!plan.clear_existing_waiting);

    // Invalid command identity must not advance evaluation cadence.
    state = base_state();
    state.report_waiting = true;
    state.wait_started_ms = 310000U;
    state.next_command_id = 0U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);
    CHECK(!plan.update_last_eval);

    state = base_state();
    state.report_waiting = true;
    state.wait_started_ms = 310000U;
    state.policy_generation = 0U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);
    CHECK(!plan.update_last_eval);

    state.policy_generation = RF_CQ_GENERATION_ANY;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);
    CHECK(!plan.update_last_eval);
}

static void test_periodic_query(void)
{
    rf_background_execution_state_t state = base_state();
    rf_background_execution_plan_t plan;

    state.current_power_dbm = RF_BG_SET_POWER_UNKNOWN_DBM;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_READY_QUERY);
    CHECK(plan.background_active);
    CHECK(plan.command.command == RF_CQ_COMMAND_QUERY);
    CHECK(plan.command.source == RF_CQ_SOURCE_PERIODIC);

    state = base_state();
    state.confirmed_ms = 100000U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_READY_QUERY);

    state.confirmed_ms = 100001U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_IDLE);
}

static void test_manual_set_and_clear(void)
{
    rf_background_execution_state_t state = base_state();
    rf_background_execution_plan_t plan;

    state.desired_mode = RF_CQ_MODE_MANUAL;
    state.desired_manual_power_dbm = 0;
    state.node_mode = RF_CQ_MODE_MANUAL;
    state.current_power_dbm = -10;
    state.policy_apply_pending = false;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_READY_SET);
    CHECK(plan.background_active);
    CHECK(plan.mark_policy_pending);
    CHECK(plan.command.source == RF_CQ_SOURCE_PERIODIC);
    CHECK(plan.command.command == RF_CQ_COMMAND_SET);
    CHECK(plan.command.desired_mode == RF_CQ_MODE_MANUAL);
    CHECK(plan.command.desired_power_dbm == 0);
    CHECK(plan.command.policy_generation == 7U);
    CHECK(plan.command.created_ms == 400000U);
    CHECK(plan.command.deadline_ms == 402000U);
    CHECK(!plan.command.ignore_backoff);
    CHECK(plan.command.request_id[0] == '\0');

    state.policy_apply_pending = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(!plan.mark_policy_pending);

    state.current_power_dbm = 0;
    state.policy_apply_pending = true;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_POLICY_ALREADY_APPLIED);
    CHECK(plan.clear_policy_pending);
    CHECK(!plan.background_active);

    state.policy_apply_pending = false;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_IDLE);
}

static void test_auto_set(void)
{
    rf_background_execution_state_t state = base_state();
    rf_background_execution_plan_t plan;

    state.desired_mode = RF_CQ_MODE_AUTO;
    state.node_mode = RF_CQ_MODE_MANUAL;
    state.current_power_dbm = -10;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_READY_SET);
    CHECK(plan.command.desired_mode == RF_CQ_MODE_AUTO);
    CHECK(plan.command.desired_power_dbm == -10);
    CHECK(plan.mark_policy_pending);

    state.node_mode = RF_BG_SET_MODE_UNKNOWN;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_IDLE);
}

static void test_invalid_policy_values(void)
{
    rf_background_execution_state_t state = base_state();
    rf_background_execution_plan_t plan;

    state.desired_mode = 2U;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);
    CHECK(!plan.update_last_eval);
    CHECK(plan.command.id == 0U);

    state = base_state();
    state.desired_mode = RF_CQ_MODE_MANUAL;
    state.desired_manual_power_dbm = 1;
    state.node_mode = RF_CQ_MODE_MANUAL;
    state.current_power_dbm = -10;
    CHECK(rf_background_execution_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_EXEC_INVALID_ARG);
    CHECK(!plan.update_last_eval);
}

int main(void)
{
    test_invalid_arguments();
    test_admission_rejections();
    test_idle();
    test_stale_node();
    test_waiting_due_query();
    test_periodic_query();
    test_manual_set_and_clear();
    test_auto_set();
    test_invalid_policy_values();

    if (s_failures != 0) {
        printf("rf_background_execution plan host tests: FAIL (%d)", s_failures);
        return 1;
    }
    puts("rf_background_execution plan host tests: PASS");
    return 0;
}
