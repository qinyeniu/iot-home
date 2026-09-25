#include "rf_background_command_mapper.h"

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

static rf_background_command_mapper_input_t base_input(void)
{
    rf_background_command_mapper_input_t input;

    memset(&input, 0, sizeof(input));
    input.now_ms = 400000U;
    input.last_eval_ms = 340000U;
    input.node_addr = 0x82cbU;
    input.last_rx_ms = 399000U;
    input.confirmed_ms = 399000U;
    input.current_power_dbm = -10;
    input.node_mode = RF_BG_MAPPER_MODE_AUTO;
    input.desired_mode = RF_BG_MAPPER_MODE_AUTO;
    input.desired_manual_power_dbm = 0;
    input.policy_apply_pending = false;
    input.tx_backoff_until_ms = 0U;
    input.user_intent_active = false;
    input.report_waiting = false;
    input.report_submitting = false;
    input.wait_started_ms = 0U;
    input.dispatch_active = false;
    input.background_command_active = false;
    input.active_command_id = 0U;
    input.active_command_generation = 0U;
    input.next_command_id = 50U;
    input.policy_generation = 7U;
    return input;
}

static void test_invalid_arguments(void)
{
    rf_background_command_mapper_input_t input = base_input();
    rf_background_command_mapper_output_t output;

    CHECK(!rf_background_command_mapper_map(NULL, &output));
    CHECK(!rf_background_command_mapper_map(&input, NULL));

    input.now_ms = 0U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_ARG);
}

static void test_valid_mapping(void)
{
    rf_background_command_mapper_input_t input = base_input();
    rf_background_command_mapper_output_t output;

    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_OK);
    CHECK(output.runtime.now_ms == 400000U);
    CHECK(output.runtime.last_eval_ms == 340000U);
    CHECK(output.runtime.node_addr == 0x82cbU);
    CHECK(output.runtime.last_rx_ms == 399000U);
    CHECK(output.runtime.confirmed_ms == 399000U);
    CHECK(output.runtime.current_power_dbm == -10);
    CHECK(output.runtime.node_mode == RF_CQ_MODE_AUTO);
    CHECK(output.runtime.desired_mode == RF_CQ_MODE_AUTO);
    CHECK(output.runtime.desired_manual_power_dbm == 0);
    CHECK(!output.runtime.policy_apply_pending);
    CHECK(output.runtime.tx_backoff_until_ms == 0U);
    CHECK(!output.runtime.user_intent_active);
    CHECK(!output.runtime.report_waiting);
    CHECK(!output.runtime.report_submitting);
    CHECK(output.runtime.wait_started_ms == 0U);
    CHECK(!output.runtime.dispatch_active);
    CHECK(!output.runtime.background_command_active);
    CHECK(output.runtime.active_command_id == 0U);
    CHECK(output.runtime.active_command_generation == 0U);
    CHECK(output.runtime.next_command_id == 50U);
    CHECK(output.runtime.policy_generation == 7U);

    input.current_power_dbm = RF_BG_MAPPER_POWER_UNKNOWN_DBM;
    input.node_mode = RF_BG_MAPPER_MODE_UNKNOWN;
    input.desired_mode = RF_BG_MAPPER_MODE_MANUAL;
    input.desired_manual_power_dbm = 20;
    input.policy_apply_pending = true;
    input.tx_backoff_until_ms = 500000U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_OK);
    CHECK(output.runtime.current_power_dbm ==
          RF_BG_SET_POWER_UNKNOWN_DBM);
    CHECK(output.runtime.node_mode == RF_BG_SET_MODE_UNKNOWN);
    CHECK(output.runtime.desired_mode == RF_CQ_MODE_MANUAL);
    CHECK(output.runtime.desired_manual_power_dbm == 20);
    CHECK(output.runtime.policy_apply_pending);
    CHECK(output.runtime.tx_backoff_until_ms == 500000U);
}

static void test_invalid_power_and_mode(void)
{
    rf_background_command_mapper_input_t input = base_input();
    rf_background_command_mapper_output_t output;

    input.current_power_dbm = 1;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_POWER);

    input = base_input();
    input.desired_manual_power_dbm = -128;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_POWER);

    input = base_input();
    input.node_mode = 2U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_MODE);

    input = base_input();
    input.desired_mode = RF_BG_MAPPER_MODE_UNKNOWN;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_MODE);

    input = base_input();
    input.desired_mode = 2U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_MODE);
}

static void test_waiting_and_identity_validation(void)
{
    rf_background_command_mapper_input_t input = base_input();
    rf_background_command_mapper_output_t output;

    input.report_waiting = true;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input.wait_started_ms = 390000U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_OK);
    CHECK(output.runtime.report_waiting);
    CHECK(output.runtime.wait_started_ms == 390000U);

    input.report_submitting = true;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_OK);
    CHECK(output.runtime.report_submitting);

    input = base_input();
    input.report_submitting = true;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input = base_input();
    input.next_command_id = 0U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input = base_input();
    input.policy_generation = 0U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input.policy_generation = RF_CQ_GENERATION_ANY;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);
}

static void test_background_ownership_validation(void)
{
    rf_background_command_mapper_input_t input = base_input();
    rf_background_command_mapper_output_t output;

    input.background_command_active = true;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input.active_command_id = 51U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input.active_command_generation = 7U;
    input.next_command_id = 50U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_OK);

    input.next_command_id = 51U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input.next_command_id = 52U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_OK);
    CHECK(output.runtime.next_command_id == 52U);
    CHECK(output.runtime.active_command_id == 51U);
    CHECK(output.runtime.active_command_generation == 7U);

    input = base_input();
    input.active_command_id = 51U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input = base_input();
    input.active_command_generation = 7U;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);

    input = base_input();
    input.background_command_active = true;
    input.active_command_id = 51U;
    input.active_command_generation = RF_CQ_GENERATION_ANY;
    CHECK(rf_background_command_mapper_map(&input, &output));
    CHECK(output.result == RF_BG_MAPPER_INVALID_STATE);
}

int main(void)
{
    test_invalid_arguments();
    test_valid_mapping();
    test_invalid_power_and_mode();
    test_waiting_and_identity_validation();
    test_background_ownership_validation();

    if (s_failures != 0) {
        printf("rf_background_command_mapper host tests: FAIL (%d)\n",
               s_failures);
        return 1;
    }
    puts("rf_background_command_mapper host tests: PASS");
    return 0;
}
