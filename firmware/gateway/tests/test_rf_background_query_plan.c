#include "rf_background_query_plan.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

#define CHECK(condition) do {     if (!(condition)) {         printf("CHECK failed: %s (%s:%d)", #condition, __FILE__, __LINE__);         ++s_failures;     } } while (0)

static rf_background_query_plan_state_t base_state(void)
{
    rf_background_query_plan_state_t state = {
        .now_ms = 400000U,
        .node_addr = 0x82cbU,
        .last_rx_ms = 390000U,
        .confirmed_ms = 380000U,
        .wait_started_ms = 0U,
        .current_power_dbm = -10,
    };
    return state;
}

static void test_invalid_arguments(void)
{
    rf_background_query_plan_state_t state = base_state();
    rf_background_query_plan_t plan;

    CHECK(!rf_background_query_plan_create(NULL, &plan));
    CHECK(!rf_background_query_plan_create(&state, NULL));
    state.now_ms = 0U;
    CHECK(!rf_background_query_plan_create(&state, &plan));

    state = base_state();
    state.node_addr = 0U;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_INVALID_ARG);

    state = base_state();
    state.current_power_dbm = 1;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_INVALID_ARG);
}

static void test_node_stale(void)
{
    rf_background_query_plan_state_t state = base_state();
    rf_background_query_plan_t plan;

    state.last_rx_ms = 0U;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_NODE_STALE);

    state = base_state();
    state.last_rx_ms = state.now_ms - RF_BG_NODE_FRESH_MS - 1U;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_NODE_STALE);

    state.last_rx_ms = state.now_ms - RF_BG_NODE_FRESH_MS;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result != RF_BG_QUERY_PLAN_NODE_STALE);

    state.now_ms = 120001U;
    state.last_rx_ms = UINT32_MAX - 99U;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_NODE_STALE);
}

static void test_waiting(void)
{
    rf_background_query_plan_state_t state = base_state();
    rf_background_query_plan_t plan;

    state.wait_started_ms = state.now_ms - RF_BG_WAIT_SETTLE_MS + 1U;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_NO_SEND_WAITING);

    state.wait_started_ms = state.now_ms - RF_BG_WAIT_SETTLE_MS;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_SEND);
}

static void test_unknown_power_and_reconcile(void)
{
    rf_background_query_plan_state_t state = base_state();
    rf_background_query_plan_t plan;

    state.current_power_dbm = RF_BG_POWER_UNKNOWN_DBM;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_SEND);

    state = base_state();
    state.confirmed_ms = 0U;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_NO_SEND);

    state.confirmed_ms = state.now_ms - RF_BG_RECONCILE_MS + 1U;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_NO_SEND);

    state.confirmed_ms = state.now_ms - RF_BG_RECONCILE_MS;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_SEND);

    state.now_ms = 50U;
    state.confirmed_ms = UINT32_MAX - RF_BG_RECONCILE_MS + 51U;
    state.last_rx_ms = state.now_ms - 1U;
    CHECK(rf_background_query_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_QUERY_PLAN_SEND);
}

int main(void)
{
    test_invalid_arguments();
    test_node_stale();
    test_waiting();
    test_unknown_power_and_reconcile();

    if (s_failures != 0) {
        printf("rf_background_query_plan host tests: FAIL (%d)", s_failures);
        return 1;
    }
    puts("rf_background_query_plan host tests: PASS");
    return 0;
}
