#include "rf_background_status_plan.h"

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

static rf_dispatch_cmd_t periodic_query_cmd(void)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_periodic_query(42U, 7U, 500000U, &cmd) == RF_CQ_OK);
    return cmd;
}

static rf_dispatch_cmd_t periodic_manual_set_cmd(void)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_periodic_set(43U, RF_CQ_MODE_MANUAL, 0, 7U,
                                   500000U, &cmd) == RF_CQ_OK);
    return cmd;
}

static rf_dispatch_cmd_t periodic_auto_set_cmd(void)
{
    rf_dispatch_cmd_t cmd;

    CHECK(rf_cq_build_periodic_set(44U, RF_CQ_MODE_AUTO, -10, 7U,
                                   500000U, &cmd) == RF_CQ_OK);
    return cmd;
}

static rf_background_status_state_t state_for(rf_dispatch_cmd_t command)
{
    rf_background_status_state_t state = {
        .now_ms = 500000U,
        .command = command,
        .reported_power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM,
        .reported_mode = RF_DISPATCH_MODE_UNKNOWN,
        .notice_allowed = true,
    };

    return state;
}

static void test_invalid_arguments(void)
{
    rf_background_status_state_t state = state_for(periodic_query_cmd());
    rf_background_status_plan_t plan;

    CHECK(!rf_background_status_plan_create(NULL, &plan));
    CHECK(!rf_background_status_plan_create(&state, NULL));
    state.now_ms = 0U;
    CHECK(!rf_background_status_plan_create(&state, &plan));

    state = state_for(periodic_query_cmd());
    state.command.source = RF_CQ_SOURCE_USER;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INVALID_ARG);

    state = state_for(periodic_query_cmd());
    state.command.id = 0U;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INVALID_ARG);

    state = state_for(periodic_query_cmd());
    state.event = (rf_background_status_event_t)99;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INVALID_ARG);

    state = state_for(periodic_query_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.matched_transaction = true;
    state.reported_mode = RF_CQ_MODE_MANUAL;
    state.reported_power_dbm = RF_DISPATCH_POWER_UNKNOWN_DBM;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INVALID_ARG);

    state = state_for(periodic_query_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.matched_transaction = true;
    state.reported_mode = RF_DISPATCH_MODE_UNKNOWN;
    state.reported_power_dbm = 0;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INVALID_ARG);

    state = state_for(periodic_query_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INVALID_ARG);

    state = state_for(periodic_query_cmd());
    state.event = RF_BG_STATUS_EVENT_WAIT_TIMEOUT;
    state.matched_transaction = true;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INVALID_ARG);
}

static void test_matched_query(void)
{
    rf_background_status_state_t state = state_for(periodic_query_cmd());
    rf_background_status_plan_t plan;

    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.matched_transaction = true;
    state.reported_mode = RF_CQ_MODE_AUTO;
    state.reported_power_dbm = -10;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_PUBLISH);
    CHECK(plan.publish_status == RF_BG_STATUS_PUBLISH_REPORT_RECEIVED);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_NONE);
    CHECK(plan.clear_background);
    CHECK(!plan.set_policy_pending);
    CHECK(plan.reported_mode == RF_CQ_MODE_AUTO);
    CHECK(plan.reported_power_dbm == -10);

    state = state_for(periodic_query_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.matched_transaction = true;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_NO_ACTION);
    CHECK(!plan.clear_background);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_NONE);

    state = state_for(periodic_query_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.node_initiated_report = true;
    state.reported_mode = RF_CQ_MODE_AUTO;
    state.reported_power_dbm = -10;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_NO_ACTION);
    CHECK(!plan.clear_background);
}

static void test_matched_set(void)
{
    rf_background_status_state_t state = state_for(periodic_manual_set_cmd());
    rf_background_status_plan_t plan;

    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.matched_transaction = true;
    state.policy_apply_pending = true;
    state.reported_mode = RF_CQ_MODE_MANUAL;
    state.reported_power_dbm = 0;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_PUBLISH);
    CHECK(plan.publish_status == RF_BG_STATUS_PUBLISH_CONFIRMED);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_ACKNOWLEDGED);
    CHECK(plan.clear_background);
    CHECK(plan.set_policy_pending);
    CHECK(!plan.policy_pending_value);
    CHECK(plan.reported_mode == RF_CQ_MODE_MANUAL);
    CHECK(plan.reported_power_dbm == 0);

    state = state_for(periodic_auto_set_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.matched_transaction = true;
    state.policy_apply_pending = true;
    state.reported_mode = RF_CQ_MODE_AUTO;
    state.reported_power_dbm = -10;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.publish_status == RF_BG_STATUS_PUBLISH_CONFIRMED);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_ACKNOWLEDGED);

    state = state_for(periodic_manual_set_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.matched_transaction = true;
    state.policy_apply_pending = true;
    state.reported_mode = RF_CQ_MODE_MANUAL;
    state.reported_power_dbm = -10;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_NO_ACTION);
    CHECK(!plan.clear_background);
    CHECK(!plan.set_policy_pending);

    state = state_for(periodic_manual_set_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.node_initiated_report = true;
    state.policy_apply_pending = true;
    state.reported_mode = RF_CQ_MODE_MANUAL;
    state.reported_power_dbm = 0;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_PUBLISH);
    CHECK(plan.publish_status == RF_BG_STATUS_PUBLISH_CONFIRMED);

    state = state_for(periodic_manual_set_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.node_initiated_report = true;
    state.policy_apply_pending = true;
    state.reported_mode = RF_CQ_MODE_MANUAL;
    state.reported_power_dbm = -10;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_NO_ACTION);

    state = state_for(periodic_manual_set_cmd());
    state.event = RF_BG_STATUS_EVENT_MATCHED_REPORT;
    state.matched_transaction = true;
    state.reported_mode = RF_CQ_MODE_MANUAL;
    state.reported_power_dbm = 0;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_NO_ACTION);
}

static void test_wait_timeout(void)
{
    rf_background_status_state_t state = state_for(periodic_query_cmd());
    rf_background_status_plan_t plan;

    state.event = RF_BG_STATUS_EVENT_WAIT_TIMEOUT;
    state.policy_apply_pending = true;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_PUBLISH);
    CHECK(plan.publish_status == RF_BG_STATUS_PUBLISH_TIMEOUT);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_TIMEOUT);
    CHECK(plan.clear_background);
    CHECK(!plan.set_policy_pending);
    CHECK(plan.reported_mode == RF_DISPATCH_MODE_UNKNOWN);
    CHECK(plan.reported_power_dbm == RF_DISPATCH_POWER_UNKNOWN_DBM);

    state.notice_allowed = false;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INTERNAL_CLOSE);
    CHECK(plan.publish_status == RF_BG_STATUS_PUBLISH_TIMEOUT);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_TIMEOUT);
    CHECK(plan.clear_background);
    CHECK(!plan.set_policy_pending);

    state = state_for(periodic_manual_set_cmd());
    state.event = RF_BG_STATUS_EVENT_WAIT_TIMEOUT;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_PUBLISH);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_TIMEOUT);
    CHECK(plan.set_policy_pending);
    CHECK(plan.policy_pending_value);

    state.notice_allowed = false;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INTERNAL_CLOSE);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_TIMEOUT);
    CHECK(plan.set_policy_pending);
    CHECK(plan.policy_pending_value);
}

static void test_send_failed(void)
{
    rf_background_status_state_t state = state_for(periodic_query_cmd());
    rf_background_status_plan_t plan;

    state.event = RF_BG_STATUS_EVENT_SEND_FAILED;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_PUBLISH);
    CHECK(plan.publish_status == RF_BG_STATUS_PUBLISH_FAILED);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_FAILED);
    CHECK(plan.clear_background);
    CHECK(!plan.set_policy_pending);

    state = state_for(periodic_manual_set_cmd());
    state.event = RF_BG_STATUS_EVENT_SEND_FAILED;
    state.notice_allowed = false;
    CHECK(rf_background_status_plan_create(&state, &plan));
    CHECK(plan.result == RF_BG_STATUS_PLAN_INTERNAL_CLOSE);
    CHECK(plan.publish_status == RF_BG_STATUS_PUBLISH_FAILED);
    CHECK(plan.table_action == RF_BG_COMMAND_TABLE_FAILED);
    CHECK(plan.clear_background);
    CHECK(plan.set_policy_pending);
    CHECK(plan.policy_pending_value);
}

int main(void)
{
    test_invalid_arguments();
    test_matched_query();
    test_matched_set();
    test_wait_timeout();
    test_send_failed();

    if (s_failures != 0) {
        printf("rf_background_status_plan host tests: FAIL (%d)\n",
               s_failures);
        return 1;
    }
    puts("rf_background_status_plan host tests: PASS");
    return 0;
}
