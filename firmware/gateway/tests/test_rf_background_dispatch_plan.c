#include "rf_background_dispatch_plan.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(condition) do {                                          \
    if (!(condition)) {                                                \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,     \
               __LINE__);                                              \
        ++s_failures;                                                  \
    }                                                                  \
} while (0)

static rf_dispatch_cmd_t periodic_query(uint32_t now_ms)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_periodic_query(42U, 7U, now_ms, &cmd) == RF_CQ_OK);
    return cmd;
}

static rf_dispatch_cmd_t periodic_manual_set(uint32_t now_ms)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_periodic_set(43U, RF_CQ_MODE_MANUAL, 0, 7U,
                                   now_ms, &cmd) == RF_CQ_OK);
    return cmd;
}

static rf_dispatch_cmd_t periodic_auto_set(uint32_t now_ms)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_periodic_set(44U, RF_CQ_MODE_AUTO, -10, 7U,
                                   now_ms, &cmd) == RF_CQ_OK);
    return cmd;
}

static rf_background_dispatch_state_t base_state(void)
{
    rf_background_dispatch_state_t state;

    memset(&state, 0, sizeof(state));
    state.now_ms = 400000U;
    state.node_addr = 0x82cbU;
    state.packet_sequence = 10U;
    state.active_command_id = 42U;
    state.active_command_generation = 7U;
    state.user_intent_active = false;
    state.report_waiting = false;
    state.report_submitting = false;
    state.dispatch_active = false;
    return state;
}

static void test_invalid_arguments(void)
{
    rf_dispatch_cmd_t cmd = periodic_query(400000U);
    rf_background_dispatch_state_t state = base_state();
    rf_background_dispatch_plan_t plan;

    CHECK(!rf_background_dispatch_plan_create(NULL, &state, &plan));
    CHECK(!rf_background_dispatch_plan_create(&cmd, NULL, &plan));
    CHECK(!rf_background_dispatch_plan_create(&cmd, &state, NULL));

    cmd.source = RF_CQ_SOURCE_USER;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_INVALID_ARG);

    cmd = periodic_query(400000U);
    cmd.id = 0U;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_INVALID_ARG);

    cmd = periodic_query(400000U);
    cmd.policy_generation = 0U;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_INVALID_ARG);
}

static void test_send_query(void)
{
    rf_dispatch_cmd_t cmd = periodic_query(400000U);
    rf_background_dispatch_state_t state = base_state();
    rf_background_dispatch_plan_t plan;

    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_SEND);
    CHECK(plan.node_addr == 0x82cbU);
    CHECK(plan.seq == 11U);
    CHECK(plan.command == RF_CQ_COMMAND_QUERY);
    CHECK(plan.packet_power_dbm == 0);
    CHECK(plan.packet_mode == RF_CQ_MODE_AUTO);
    CHECK(plan.expected_report_power_dbm ==
          RF_BG_DISPATCH_POWER_UNKNOWN_DBM);
    CHECK(plan.expected_report_mode == RF_BG_DISPATCH_MODE_UNKNOWN);
}

static void test_send_set(void)
{
    rf_background_dispatch_state_t state = base_state();
    rf_background_dispatch_plan_t plan;

    rf_dispatch_cmd_t manual = periodic_manual_set(400000U);
    state.active_command_id = 43U;
    CHECK(rf_background_dispatch_plan_create(&manual, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_SEND);
    CHECK(plan.seq == 11U);
    CHECK(plan.command == RF_CQ_COMMAND_SET);
    CHECK(plan.packet_power_dbm == 0);
    CHECK(plan.packet_mode == RF_CQ_MODE_MANUAL);
    CHECK(plan.expected_report_power_dbm == 0);
    CHECK(plan.expected_report_mode == RF_CQ_MODE_MANUAL);

    rf_dispatch_cmd_t automatic = periodic_auto_set(400000U);
    state.active_command_id = 44U;
    CHECK(rf_background_dispatch_plan_create(&automatic, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_SEND);
    CHECK(plan.packet_power_dbm == -10);
    CHECK(plan.packet_mode == RF_CQ_MODE_AUTO);
    CHECK(plan.expected_report_power_dbm == -10);
    CHECK(plan.expected_report_mode == RF_CQ_MODE_AUTO);
}

static void test_sequence_wrapping(void)
{
    rf_dispatch_cmd_t cmd = periodic_query(400000U);
    rf_background_dispatch_state_t state = base_state();
    rf_background_dispatch_plan_t plan;
    uint8_t sequences[] = {0U, 127U, 255U};

    state.packet_sequence = 126U;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_SEND);
    CHECK(plan.seq == 127U);

    for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); ++i) {
        state.packet_sequence = sequences[i];
        CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
        CHECK(plan.status == RF_BG_DISPATCH_SEND);
        CHECK(plan.seq == 1U);
    }
}

static void test_deadline_wrapping(void)
{
    rf_dispatch_cmd_t cmd = periodic_query(400000U);
    rf_background_dispatch_state_t state = base_state();
    rf_background_dispatch_plan_t plan;

    cmd.created_ms = 0xfffff000U;
    cmd.deadline_ms = 0x000003e8U;
    state.now_ms = 0xffffffffU;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_SEND);

    state.now_ms = 0x000003e8U;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_CLOSE_EXPIRED);
}

static void test_stale_identity_and_expiry(void)
{
    rf_dispatch_cmd_t cmd = periodic_query(400000U);
    rf_background_dispatch_state_t state = base_state();
    rf_background_dispatch_plan_t plan;

    state.active_command_id = 99U;
    state.now_ms = 402000U;
    state.user_intent_active = true;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_DROP_STALE);
    CHECK(plan.seq == 0U);
    state.user_intent_active = false;

    state.active_command_id = 42U;
    state.active_command_generation = 8U;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_DROP_STALE);

    state.active_command_generation = 7U;
    state.now_ms = 402000U;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_CLOSE_EXPIRED);

    state.now_ms = 401999U;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_SEND);
}

static void test_state_changed_and_inactive(void)
{
    rf_dispatch_cmd_t cmd = periodic_query(400000U);
    rf_background_dispatch_state_t state = base_state();
    rf_background_dispatch_plan_t plan;

    state.user_intent_active = true;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_CLOSE_STATE_CHANGED);

    state.user_intent_active = false;
    state.report_waiting = true;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_CLOSE_STATE_CHANGED);

    state.report_waiting = false;
    state.report_submitting = true;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_CLOSE_STATE_CHANGED);

    state.report_submitting = false;
    state.dispatch_active = true;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_CLOSE_STATE_CHANGED);

    state.dispatch_active = false;
    state.node_addr = 0U;
    CHECK(rf_background_dispatch_plan_create(&cmd, &state, &plan));
    CHECK(plan.status == RF_BG_DISPATCH_CLOSE_INACTIVE);
}

int main(void)
{
    test_invalid_arguments();
    test_send_query();
    test_send_set();
    test_sequence_wrapping();
    test_deadline_wrapping();
    test_stale_identity_and_expiry();
    test_state_changed_and_inactive();

    if (s_failures != 0) {
        printf("rf_background_dispatch_plan host tests: FAIL (%d)\n",
               s_failures);
        return 1;
    }
    puts("rf_background_dispatch_plan host tests: PASS");
    return 0;
}
