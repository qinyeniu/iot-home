#include "rf_background_set_plan.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

#define CHECK(condition) do {     if (!(condition)) {         printf("CHECK failed: %s (%s:%d)", #condition, __FILE__, __LINE__);         ++s_failures;     } } while (0)

static rf_background_set_plan_state_t base_state(void)
{
    rf_background_set_plan_state_t state = {
        .now_ms = 400000U,
        .node_addr = 0x82cbU,
        .current_power_dbm = -10,
        .node_mode = RF_CQ_MODE_AUTO,
        .desired_mode = RF_CQ_MODE_AUTO,
        .desired_manual_power_dbm = 0,
        .policy_apply_pending = false,
    };
    return state;
}

static void test_invalid_arguments(void)
{
    rf_background_set_plan_state_t state = base_state();
    rf_background_set_plan_t plan;

    CHECK(!rf_background_set_plan_create(NULL, &plan));
    CHECK(!rf_background_set_plan_create(&state, NULL));
    state.now_ms = 0U;
    CHECK(!rf_background_set_plan_create(&state, &plan));

    state = base_state();
    state.node_addr = 0U;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_INVALID_ARG);

    state = base_state();
    state.current_power_dbm = 1;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_INVALID_ARG);

    state = base_state();
    state.current_power_dbm = RF_BG_SET_POWER_UNKNOWN_DBM;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_INVALID_ARG);

    state = base_state();
    state.node_mode = 2U;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_INVALID_ARG);

    state = base_state();
    state.desired_mode = 2U;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_INVALID_ARG);

    state = base_state();
    state.desired_mode = RF_CQ_MODE_MANUAL;
    state.desired_manual_power_dbm = 1;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_INVALID_ARG);
}

static void test_manual_matches(void)
{
    rf_background_set_plan_state_t state = base_state();
    rf_background_set_plan_t plan;

    state.desired_mode = RF_CQ_MODE_MANUAL;
    state.desired_manual_power_dbm = 0;
    state.current_power_dbm = 0;
    state.node_mode = RF_CQ_MODE_MANUAL;
    state.policy_apply_pending = false;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_NO_SEND);

    state.policy_apply_pending = true;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_CLEAR_PENDING);
}

static void test_manual_mismatch(void)
{
    rf_background_set_plan_state_t state = base_state();
    rf_background_set_plan_t plan;

    state.desired_mode = RF_CQ_MODE_MANUAL;
    state.desired_manual_power_dbm = 0;
    state.current_power_dbm = -10;
    state.node_mode = RF_CQ_MODE_MANUAL;
    state.policy_apply_pending = true;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_SEND);
    CHECK(plan.set_power_dbm == 0);
    CHECK(plan.set_mode == RF_CQ_MODE_MANUAL);
    CHECK(!plan.mark_policy_pending);

    state.policy_apply_pending = false;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_SEND);
    CHECK(plan.mark_policy_pending);

    state.node_mode = RF_CQ_MODE_AUTO;
    state.policy_apply_pending = true;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_SEND);
    CHECK(plan.set_power_dbm == 0);
    CHECK(plan.set_mode == RF_CQ_MODE_MANUAL);

    state.node_mode = RF_BG_SET_MODE_UNKNOWN;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_NO_SEND);
}

static void test_auto_matches(void)
{
    rf_background_set_plan_state_t state = base_state();
    rf_background_set_plan_t plan;

    state.desired_mode = RF_CQ_MODE_AUTO;
    state.node_mode = RF_CQ_MODE_AUTO;
    state.policy_apply_pending = false;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_NO_SEND);

    state.policy_apply_pending = true;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_CLEAR_PENDING);
}

static void test_auto_mismatch(void)
{
    rf_background_set_plan_state_t state = base_state();
    rf_background_set_plan_t plan;

    state.desired_mode = RF_CQ_MODE_AUTO;
    state.node_mode = RF_CQ_MODE_MANUAL;
    state.current_power_dbm = -10;
    state.policy_apply_pending = true;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_SEND);
    CHECK(plan.set_power_dbm == -10);
    CHECK(plan.set_mode == RF_CQ_MODE_AUTO);
    CHECK(!plan.mark_policy_pending);

    state.policy_apply_pending = false;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.mark_policy_pending);

    state.node_mode = RF_BG_SET_MODE_UNKNOWN;
    CHECK(rf_background_set_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_SET_PLAN_NO_SEND);
}

int main(void)
{
    test_invalid_arguments();
    test_manual_matches();
    test_manual_mismatch();
    test_auto_matches();
    test_auto_mismatch();

    if (s_failures != 0) {
        printf("rf_background_set_plan host tests: FAIL (%d)", s_failures);
        return 1;
    }
    puts("rf_background_set_plan host tests: PASS");
    return 0;
}
