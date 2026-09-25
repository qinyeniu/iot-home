#include "rf_dispatch_plan.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

#define CHECK(condition) do {                                          \
    if (!(condition)) {                                                \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,     \
               __LINE__);                                              \
        ++s_failures;                                                  \
    }                                                                  \
} while (0)

static rf_dispatch_plan_state_t base_state(void)
{
    rf_dispatch_plan_state_t state;

    state.now_ms = 100000U;
    state.node_addr = 0x1234U;
    state.node_power_dbm = -10;
    state.report_waiting = false;
    state.wait_deadline_ms = 120000U;
    state.packet_sequence = 10U;
    return state;
}

static rf_dispatch_cmd_t user_query(uint32_t now_ms)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_user_query(1, "0123456789abcdef", now_ms,
                                 &cmd) == RF_CQ_OK);
    return cmd;
}

static rf_dispatch_cmd_t user_manual(int8_t target_power, uint32_t now_ms)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_user_manual_set(1, target_power, "0123456789abcdef",
                                      now_ms, &cmd) == RF_CQ_OK);
    return cmd;
}

static rf_dispatch_cmd_t user_auto(int8_t placeholder_power, uint32_t now_ms)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_user_auto_set(1, placeholder_power,
                                    "0123456789abcdef", now_ms,
                                    &cmd) == RF_CQ_OK);
    return cmd;
}

static void test_invalid_arguments(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_query(state.now_ms);
    rf_dispatch_plan_t plan;

    CHECK(!rf_dispatch_plan_create(NULL, &state, &plan));
    CHECK(!rf_dispatch_plan_create(&cmd, NULL, &plan));
    CHECK(!rf_dispatch_plan_create(&cmd, &state, NULL));

    state.node_power_dbm = 1;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_ERR_INVALID_ARG);
}

static void test_query_plan(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_query(state.now_ms);
    rf_dispatch_plan_t plan;

    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_SEND);
    CHECK(plan.node_addr == 0x1234U);
    CHECK(plan.seq == 11U);
    CHECK(plan.command == RF_DISPATCH_COMMAND_QUERY);
    CHECK(plan.power_dbm == 0);
    CHECK(plan.mode == RF_DISPATCH_MODE_AUTO);
    CHECK(plan.publish_querying);
    CHECK(!plan.clear_waiting);
    CHECK(plan.expected_report_power_dbm == RF_DISPATCH_POWER_UNKNOWN_DBM);
    CHECK(plan.expected_report_mode == RF_DISPATCH_MODE_UNKNOWN);
}

static void test_query_sequence_wrapping(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_query(state.now_ms);
    rf_dispatch_plan_t plan;

    state.packet_sequence = 0U;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.seq == 1U);

    state.packet_sequence = 126U;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.seq == 127U);

    state.packet_sequence = 127U;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.seq == 1U);

    state.packet_sequence = 255U;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.seq == 1U);
}

static void test_query_waiting_and_inactive(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_query(state.now_ms);
    rf_dispatch_plan_t plan;

    state.report_waiting = true;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_ERR_WAITING);

    state.wait_deadline_ms = state.now_ms;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_SEND);
    CHECK(plan.seq == 11U);

    state = base_state();
    cmd = user_query(state.now_ms);
    state.node_addr = 0U;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_ERR_INACTIVE);
}

static void test_query_ttl_edges(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_query(state.now_ms - 5000U);
    rf_dispatch_plan_t plan;

    cmd.deadline_ms = state.now_ms;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_ERR_EXPIRED);

    state.now_ms = UINT32_MAX - 100U;
    cmd = user_query(state.now_ms);
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_SEND);
    CHECK(plan.seq == 11U);
}

static void test_manual_plan(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_manual(0, state.now_ms);
    rf_dispatch_plan_t plan;

    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_SEND);
    CHECK(plan.seq == 11U);
    CHECK(plan.command == RF_DISPATCH_COMMAND_SET);
    CHECK(plan.power_dbm == 0);
    CHECK(plan.mode == RF_DISPATCH_MODE_MANUAL);
    CHECK(!plan.publish_querying);
    CHECK(plan.clear_waiting);
    CHECK(plan.expected_report_power_dbm == 0);
    CHECK(plan.expected_report_mode == RF_DISPATCH_MODE_MANUAL);

    cmd.desired_power_dbm = 1;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_ERR_INVALID_ARG);
}

static void test_manual_special_states(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_manual(0, state.now_ms);
    rf_dispatch_plan_t plan;

    state.node_addr = 0U;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_NO_SEND_POLICY_SAVED);
    CHECK(plan.seq == 0U);
    CHECK(plan.clear_waiting);

    state = base_state();
    cmd = user_manual(0, state.now_ms);
    state.node_power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_SEND);
    CHECK(plan.command == RF_DISPATCH_COMMAND_QUERY);
    CHECK(!plan.publish_querying);
    CHECK(plan.clear_waiting);
    CHECK(plan.expected_report_power_dbm == RF_DISPATCH_POWER_UNKNOWN_DBM);
    CHECK(plan.expected_report_mode == RF_DISPATCH_MODE_UNKNOWN);

    state = base_state();
    cmd = user_manual(0, state.now_ms);
    state.report_waiting = true;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_SEND);
    CHECK(plan.command == RF_DISPATCH_COMMAND_SET);
    CHECK(plan.clear_waiting);
}

static void test_auto_plan_uses_current_power(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_auto(0, state.now_ms);
    rf_dispatch_plan_t plan;

    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_SEND);
    CHECK(plan.seq == 11U);
    CHECK(plan.command == RF_DISPATCH_COMMAND_SET);
    CHECK(plan.power_dbm == -10);
    CHECK(plan.mode == RF_DISPATCH_MODE_AUTO);
    CHECK(plan.clear_waiting);
    CHECK(plan.expected_report_power_dbm == -10);
    CHECK(plan.expected_report_mode == RF_DISPATCH_MODE_AUTO);

    state.node_power_dbm = 20;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.power_dbm == 20);
    CHECK(plan.expected_report_power_dbm == 20);
}

static void test_auto_special_states(void)
{
    rf_dispatch_plan_state_t state = base_state();
    rf_dispatch_cmd_t cmd = user_auto(0, state.now_ms);
    rf_dispatch_plan_t plan;

    state.node_addr = 0U;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_NO_SEND_POLICY_SAVED);
    CHECK(plan.clear_waiting);

    state = base_state();
    cmd = user_auto(0, state.now_ms);
    state.node_power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_NO_SEND_POLICY_SAVED);
    CHECK(plan.clear_waiting);

    state = base_state();
    cmd = user_auto(0, state.now_ms);
    state.report_waiting = true;
    CHECK(rf_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_DISPATCH_PLAN_SEND);
    CHECK(plan.command == RF_DISPATCH_COMMAND_SET);
    CHECK(plan.clear_waiting);
}

static rf_dispatch_after_report_state_t base_after_report_state(void)
{
    rf_dispatch_after_report_state_t state = {
        .intent_command = RF_DISPATCH_COMMAND_SET,
        .desired_mode = RF_DISPATCH_MODE_MANUAL,
        .target_power_dbm = 0,
        .policy_apply_pending = true,
        .reported_power_dbm = -10,
        .reported_mode = RF_DISPATCH_MODE_MANUAL,
        .matched_transaction = false,
        .node_initiated_report = false,
    };
    return state;
}

static void test_after_report_policy(void)
{
    rf_dispatch_after_report_state_t state = base_after_report_state();
    rf_dispatch_after_report_result_t result;

    CHECK(!rf_dispatch_after_report_create(NULL, &result));
    CHECK(!rf_dispatch_after_report_create(&state, NULL));

    state.intent_command = RF_DISPATCH_COMMAND_QUERY;
    state.matched_transaction = true;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_COMPLETE);

    state.matched_transaction = false;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_NONE);

    state = base_after_report_state();
    state.matched_transaction = true;
    state.reported_power_dbm = 0;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_COMPLETE);

    state = base_after_report_state();
    state.matched_transaction = true;
    state.desired_mode = RF_DISPATCH_MODE_AUTO;
    state.reported_mode = RF_DISPATCH_MODE_AUTO;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_COMPLETE);

    state = base_after_report_state();
    state.matched_transaction = true;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_RETRY);

    state = base_after_report_state();
    state.matched_transaction = false;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_NONE);

    state.node_initiated_report = true;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_NONE);

    state.reported_power_dbm = 0;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_COMPLETE);

    state = base_after_report_state();
    state.node_initiated_report = true;
    state.desired_mode = RF_DISPATCH_MODE_AUTO;
    state.reported_mode = RF_DISPATCH_MODE_AUTO;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_COMPLETE);

    state = base_after_report_state();
    state.reported_power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_NONE);

    state = base_after_report_state();
    state.policy_apply_pending = false;
    CHECK(rf_dispatch_after_report_create(&state, &result));
    CHECK(result == RF_DISPATCH_AFTER_REPORT_NONE);
}

int main(void)
{
    test_invalid_arguments();
    test_query_plan();
    test_query_sequence_wrapping();
    test_query_waiting_and_inactive();
    test_query_ttl_edges();
    test_manual_plan();
    test_manual_special_states();
    test_auto_plan_uses_current_power();
    test_auto_special_states();
    test_after_report_policy();

    if (s_failures != 0) {
        printf("rf_dispatch_plan host tests: FAIL (%d)\n", s_failures);
        return 1;
    }
    puts("rf_dispatch_plan host tests: PASS");
    return 0;
}