#include "rf_command_queue.h"

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

static rf_dispatch_cmd_t make_cmd(rf_cq_source_t source,
                                  rf_cq_command_t command,
                                  rf_cq_mode_t mode, int8_t power,
                                  uint32_t generation)
{
    rf_dispatch_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.id = 1;
    cmd.source = source;
    cmd.command = command;
    cmd.desired_mode = mode;
    cmd.desired_power_dbm = power;
    cmd.policy_generation = generation;
    cmd.created_ms = 1000;
    cmd.deadline_ms = 5000;
    cmd.ignore_backoff = source == RF_CQ_SOURCE_USER;
    memcpy(cmd.request_id, "0123456789abcdef", RF_CQ_REQUEST_ID_LEN);
    return cmd;
}

static void test_request_id_and_validation(void)
{
    rf_dispatch_cmd_t query = make_cmd(RF_CQ_SOURCE_USER,
                                      RF_CQ_COMMAND_QUERY,
                                      RF_CQ_MODE_AUTO, 0,
                                      RF_CQ_GENERATION_ANY);
    rf_dispatch_cmd_t manual = make_cmd(RF_CQ_SOURCE_USER,
                                        RF_CQ_COMMAND_SET,
                                        RF_CQ_MODE_MANUAL, 0,
                                        RF_CQ_GENERATION_ANY);
    rf_dispatch_cmd_t automatic = make_cmd(RF_CQ_SOURCE_AUTO,
                                           RF_CQ_COMMAND_SET,
                                           RF_CQ_MODE_MANUAL, -10, 10);
    rf_dispatch_cmd_t bad_power = make_cmd(RF_CQ_SOURCE_USER,
                                           RF_CQ_COMMAND_SET,
                                           RF_CQ_MODE_MANUAL, 1,
                                           RF_CQ_GENERATION_ANY);
    rf_dispatch_cmd_t bad_query = make_cmd(RF_CQ_SOURCE_USER,
                                           RF_CQ_COMMAND_QUERY,
                                           RF_CQ_MODE_MANUAL, 0,
                                           RF_CQ_GENERATION_ANY);
    rf_dispatch_cmd_t bad_periodic = make_cmd(RF_CQ_SOURCE_PERIODIC,
                                              RF_CQ_COMMAND_QUERY,
                                              RF_CQ_MODE_AUTO, 0,
                                              RF_CQ_GENERATION_ANY);
    rf_dispatch_cmd_t empty_periodic = make_cmd(RF_CQ_SOURCE_PERIODIC,
                                               RF_CQ_COMMAND_QUERY,
                                               RF_CQ_MODE_AUTO, 0, 10);
    rf_dispatch_cmd_t empty_auto = make_cmd(RF_CQ_SOURCE_AUTO,
                                           RF_CQ_COMMAND_SET,
                                           RF_CQ_MODE_AUTO, -10, 10);

    CHECK(rf_cq_request_id_valid("0123456789abcdef"));
    CHECK(!rf_cq_request_id_valid("0123456789abcde"));
    CHECK(!rf_cq_request_id_valid("0123456789abcdeG"));
    CHECK(!rf_cq_request_id_valid(NULL));
    CHECK(rf_cq_supported_power(-10));
    CHECK(rf_cq_supported_power(20));
    CHECK(!rf_cq_supported_power(1));
    CHECK(rf_cq_validate(&query) == RF_CQ_OK);
    CHECK(rf_cq_validate(&manual) == RF_CQ_OK);
    CHECK(rf_cq_validate(&automatic) == RF_CQ_OK);
    CHECK(rf_cq_validate(&bad_power) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_validate(&bad_query) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_validate(&bad_periodic) == RF_CQ_ERR_INVALID_ARG);

    empty_periodic.request_id[0] = '\0';
    empty_auto.request_id[0] = '\0';
    CHECK(rf_cq_validate(&empty_periodic) == RF_CQ_OK);
    CHECK(rf_cq_validate(&empty_auto) == RF_CQ_OK);
    empty_periodic.request_id[0] = 'x';
    CHECK(rf_cq_validate(&empty_periodic) == RF_CQ_ERR_INVALID_ARG);

    automatic.ignore_backoff = true;
    CHECK(rf_cq_validate(&automatic) == RF_CQ_ERR_INVALID_ARG);
    automatic.ignore_backoff = false;
    automatic.policy_generation = RF_CQ_GENERATION_ANY;
    CHECK(rf_cq_validate(&automatic) == RF_CQ_ERR_INVALID_ARG);
}

static void test_user_command_builders(void)
{
    rf_dispatch_cmd_t query;
    rf_dispatch_cmd_t manual;
    rf_dispatch_cmd_t automatic;

    CHECK(rf_cq_build_user_query(1, "0123456789abcdef", 1000,
                                 &query) == RF_CQ_OK);
    CHECK(query.id == 1);
    CHECK(query.source == RF_CQ_SOURCE_USER);
    CHECK(query.command == RF_CQ_COMMAND_QUERY);
    CHECK(query.desired_mode == RF_CQ_MODE_AUTO);
    CHECK(query.desired_power_dbm == 0);
    CHECK(query.policy_generation == RF_CQ_GENERATION_ANY);
    CHECK(query.created_ms == 1000);
    CHECK(query.deadline_ms == 1000 + RF_CQ_USER_TTL_MS);
    CHECK(query.ignore_backoff);
    CHECK(strcmp(query.request_id, "0123456789abcdef") == 0);

    CHECK(rf_cq_build_user_manual_set(2, 0, "0123456789abcdef", 2000,
                                      &manual) == RF_CQ_OK);
    CHECK(manual.command == RF_CQ_COMMAND_SET);
    CHECK(manual.desired_mode == RF_CQ_MODE_MANUAL);
    CHECK(manual.desired_power_dbm == 0);
    CHECK(manual.created_ms == 2000);
    CHECK(manual.deadline_ms == 2000 + RF_CQ_USER_TTL_MS);

    CHECK(rf_cq_build_user_auto_set(3, -10, "0123456789abcdef", 3000,
                                    &automatic) == RF_CQ_OK);
    CHECK(automatic.command == RF_CQ_COMMAND_SET);
    CHECK(automatic.desired_mode == RF_CQ_MODE_AUTO);
    CHECK(automatic.desired_power_dbm == -10);
    CHECK(automatic.created_ms == 3000);
    CHECK(automatic.deadline_ms == 3000 + RF_CQ_USER_TTL_MS);

    CHECK(rf_cq_build_user_query(0, "0123456789abcdef", 1000,
                                 &query) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_user_query(1, "0123456789abcdef", 0,
                                 &query) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_user_query(1, "0123456789abcde", 1000,
                                 &query) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_user_query(1, "0123456789abcdef", 1000,
                                 NULL) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_user_manual_set(2, 1, "0123456789abcdef", 2000,
                                      &manual) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_user_auto_set(3, 1, "0123456789abcdef", 3000,
                                    &automatic) == RF_CQ_ERR_INVALID_ARG);
}

static void test_background_command_builders(void)
{
    rf_dispatch_cmd_t query;
    rf_dispatch_cmd_t periodic_manual;
    rf_dispatch_cmd_t periodic_auto;
    rf_dispatch_cmd_t automatic;

    CHECK(rf_cq_build_periodic_query(10, 20, 1000,
                                    &query) == RF_CQ_OK);
    CHECK(query.source == RF_CQ_SOURCE_PERIODIC);
    CHECK(query.command == RF_CQ_COMMAND_QUERY);
    CHECK(query.desired_mode == RF_CQ_MODE_AUTO);
    CHECK(query.desired_power_dbm == 0);
    CHECK(query.policy_generation == 20);
    CHECK(query.request_id[0] == '\0');
    CHECK(!query.ignore_backoff);
    CHECK(query.created_ms == 1000);
    CHECK(query.deadline_ms == 1000 + RF_CQ_BACKGROUND_TTL_MS);

    CHECK(rf_cq_build_periodic_set(11, RF_CQ_MODE_MANUAL, 0, 20, 2000,
                                  &periodic_manual) == RF_CQ_OK);
    CHECK(periodic_manual.source == RF_CQ_SOURCE_PERIODIC);
    CHECK(periodic_manual.command == RF_CQ_COMMAND_SET);
    CHECK(periodic_manual.desired_mode == RF_CQ_MODE_MANUAL);
    CHECK(periodic_manual.desired_power_dbm == 0);
    CHECK(periodic_manual.policy_generation == 20);

    CHECK(rf_cq_build_periodic_set(12, RF_CQ_MODE_AUTO, -10, 20, 3000,
                                  &periodic_auto) == RF_CQ_OK);
    CHECK(periodic_auto.desired_mode == RF_CQ_MODE_AUTO);
    CHECK(periodic_auto.desired_power_dbm == -10);

    CHECK(rf_cq_build_auto_set(13, -10, 20, 4000,
                              &automatic) == RF_CQ_OK);
    CHECK(automatic.source == RF_CQ_SOURCE_AUTO);
    CHECK(automatic.command == RF_CQ_COMMAND_SET);
    CHECK(automatic.desired_mode == RF_CQ_MODE_AUTO);
    CHECK(automatic.desired_power_dbm == -10);
    CHECK(automatic.policy_generation == 20);

    CHECK(rf_cq_build_periodic_query(0, 20, 1000,
                                    &query) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_periodic_query(10, 0, 1000,
                                    &query) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_periodic_query(10, RF_CQ_GENERATION_ANY, 1000,
                                    &query) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_periodic_query(10, 20, 1000,
                                    NULL) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_periodic_set(11, 2, 0, 20, 2000,
                                  &periodic_manual) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_periodic_set(11, RF_CQ_MODE_MANUAL, 1, 20, 2000,
                                  &periodic_manual) == RF_CQ_ERR_INVALID_ARG);
    CHECK(rf_cq_build_auto_set(13, 1, 20, 4000,
                              &automatic) == RF_CQ_ERR_INVALID_ARG);
}

static void test_fifo_order_capacity_and_wrap(void)
{
    rf_cq_fifo_t queue;
    rf_dispatch_cmd_t cmd = make_cmd(RF_CQ_SOURCE_USER,
                                     RF_CQ_COMMAND_QUERY,
                                     RF_CQ_MODE_AUTO, 0,
                                     RF_CQ_GENERATION_ANY);
    uint32_t i;

    rf_cq_fifo_init(&queue);
    for (i = 0; i < RF_CQ_CAPACITY; ++i) {
        cmd.id = i + 1;
        CHECK(rf_cq_fifo_push(&queue, &cmd) == RF_CQ_OK);
    }
    CHECK(rf_cq_fifo_count(&queue) == RF_CQ_CAPACITY);
    CHECK(rf_cq_fifo_push(&queue, &cmd) == RF_CQ_ERR_FULL);

    for (i = 0; i < 4; ++i) {
        CHECK(rf_cq_fifo_pop(&queue, &cmd) == RF_CQ_OK);
        CHECK(cmd.id == i + 1);
    }
    CHECK(rf_cq_fifo_count(&queue) == 4);

    /* These slots must wrap to the beginning of the circular storage. */
    for (i = 0; i < 4; ++i) {
        cmd.id = 100 + i;
        CHECK(rf_cq_fifo_push(&queue, &cmd) == RF_CQ_OK);
    }
    CHECK(rf_cq_fifo_count(&queue) == RF_CQ_CAPACITY);
    CHECK(rf_cq_fifo_push(&queue, &cmd) == RF_CQ_ERR_FULL);

    for (i = 0; i < 4; ++i) {
        CHECK(rf_cq_fifo_pop(&queue, &cmd) == RF_CQ_OK);
        CHECK(cmd.id == 5 + i);
    }
    for (i = 0; i < 4; ++i) {
        CHECK(rf_cq_fifo_pop(&queue, &cmd) == RF_CQ_OK);
        CHECK(cmd.id == 100 + i);
    }
    CHECK(rf_cq_fifo_count(&queue) == 0);
    CHECK(rf_cq_fifo_peek(&queue) == NULL);
}

static void test_periodic_merge_and_user_order(void)
{
    rf_cq_fifo_t queue;
    rf_dispatch_cmd_t first_user = make_cmd(RF_CQ_SOURCE_USER,
                                           RF_CQ_COMMAND_QUERY,
                                           RF_CQ_MODE_AUTO, 0,
                                           RF_CQ_GENERATION_ANY);
    rf_dispatch_cmd_t old_periodic = make_cmd(RF_CQ_SOURCE_PERIODIC,
                                              RF_CQ_COMMAND_QUERY,
                                              RF_CQ_MODE_AUTO, 0, 10);
    rf_dispatch_cmd_t second_user = make_cmd(RF_CQ_SOURCE_USER,
                                            RF_CQ_COMMAND_SET,
                                            RF_CQ_MODE_MANUAL, 0,
                                            RF_CQ_GENERATION_ANY);
    rf_dispatch_cmd_t new_auto = make_cmd(RF_CQ_SOURCE_AUTO,
                                          RF_CQ_COMMAND_SET,
                                          RF_CQ_MODE_MANUAL, -10, 11);
    rf_dispatch_cmd_t out;

    rf_cq_fifo_init(&queue);
    first_user.id = 100;
    old_periodic.id = 101;
    second_user.id = 102;
    new_auto.id = 103;
    CHECK(rf_cq_fifo_push(&queue, &first_user) == RF_CQ_OK);
    CHECK(rf_cq_submit_or_merge(&queue, &old_periodic) == RF_CQ_OK);
    CHECK(rf_cq_fifo_push(&queue, &second_user) == RF_CQ_OK);
    CHECK(rf_cq_submit_or_merge(&queue, &new_auto) == RF_CQ_OK);
    CHECK(rf_cq_fifo_count(&queue) == 3);

    CHECK(rf_cq_fifo_pop(&queue, &out) == RF_CQ_OK && out.id == 100);
    CHECK(rf_cq_fifo_pop(&queue, &out) == RF_CQ_OK);
    CHECK(out.id == 103);
    CHECK(out.command == RF_CQ_COMMAND_SET);
    CHECK(out.desired_power_dbm == -10);
    CHECK(out.policy_generation == 11);
    CHECK(rf_cq_fifo_pop(&queue, &out) == RF_CQ_OK && out.id == 102);
}

static void test_evaluation_rules(void)
{
    rf_dispatch_cmd_t user = make_cmd(RF_CQ_SOURCE_USER,
                                      RF_CQ_COMMAND_SET,
                                      RF_CQ_MODE_MANUAL, 0,
                                      RF_CQ_GENERATION_ANY);
    rf_dispatch_cmd_t periodic = make_cmd(RF_CQ_SOURCE_PERIODIC,
                                          RF_CQ_COMMAND_QUERY,
                                          RF_CQ_MODE_AUTO, 0, 10);
    rf_dispatch_cmd_t automatic = make_cmd(RF_CQ_SOURCE_AUTO,
                                           RF_CQ_COMMAND_SET,
                                           RF_CQ_MODE_MANUAL, -10, 10);
    rf_cq_context_t context = {
        .now_ms = 2000,
        .current_generation = 10,
        .waiting_active = false,
        .wait_deadline_ms = 4000,
        .backoff_until_ms = 3000,
    };

    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_ERR_BACKOFF);
    CHECK(rf_cq_evaluate(&automatic, &context) == RF_CQ_ERR_BACKOFF);
    CHECK(rf_cq_evaluate(&user, &context) == RF_CQ_OK);

    context.backoff_until_ms = 1000;
    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_OK);
    CHECK(rf_cq_evaluate(&automatic, &context) == RF_CQ_OK);

    periodic.policy_generation = 9;
    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_ERR_STALE_GENERATION);
    periodic.policy_generation = 10;

    context.waiting_active = true;
    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_ERR_WAITING);
    CHECK(rf_cq_evaluate(&automatic, &context) == RF_CQ_ERR_WAITING);
    CHECK(rf_cq_evaluate(&user, &context) == RF_CQ_ERR_WAITING);
    context.now_ms = 4000;
    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_OK);
    CHECK(rf_cq_evaluate(&user, &context) == RF_CQ_OK);

    context.now_ms = 5000;
    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_ERR_EXPIRED);
}

static void test_tick_wrap_and_zero_backoff(void)
{
    rf_dispatch_cmd_t periodic = make_cmd(RF_CQ_SOURCE_PERIODIC,
                                          RF_CQ_COMMAND_QUERY,
                                          RF_CQ_MODE_AUTO, 0, 10);
    rf_cq_context_t context = {
        .now_ms = UINT32_MAX - 100u,
        .current_generation = 10,
        .waiting_active = false,
        .wait_deadline_ms = 0,
        .backoff_until_ms = 0,
    };

    periodic.created_ms = UINT32_MAX - 1000u;
    periodic.deadline_ms = 1000;
    CHECK(rf_cq_validate(&periodic) == RF_CQ_OK);
    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_OK);

    /* A user TTL can legitimately wrap to deadline zero at the tick origin. */
    periodic.created_ms = UINT32_MAX - 4999u;
    periodic.deadline_ms = 0U;
    context.now_ms = UINT32_MAX - 4000u;
    CHECK(rf_cq_validate(&periodic) == RF_CQ_OK);
    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_OK);
    context.now_ms = 0U;
    CHECK(rf_cq_evaluate(&periodic, &context) == RF_CQ_ERR_EXPIRED);

    /* Zero deadline remains invalid when its relative TTL is in the past. */
    periodic.created_ms = 1000U;
    periodic.deadline_ms = 0U;
    CHECK(rf_cq_validate(&periodic) == RF_CQ_ERR_INVALID_ARG);

    CHECK(rf_cq_time_reached(UINT32_MAX, UINT32_MAX - 100));
    CHECK(rf_cq_time_reached(10, UINT32_MAX - 100));
    CHECK(!rf_cq_time_reached(UINT32_MAX - 200, UINT32_MAX - 100));
}

int main(void)
{
    test_request_id_and_validation();
    test_user_command_builders();
    test_background_command_builders();
    test_fifo_order_capacity_and_wrap();
    test_periodic_merge_and_user_order();
    test_evaluation_rules();
    test_tick_wrap_and_zero_backoff();
    if (s_failures != 0) {
        printf("rf_command_queue host tests: FAIL (%d)\n", s_failures);
        return 1;
    }
    puts("rf_command_queue host tests: PASS");
    return 0;
}
