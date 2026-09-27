#include "iot_rf_power_policy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const int8_t k_levels[] = {-10, 0, 8, 14, 18, 20};

static iot_rf_power_policy_config_t make_config(void)
{
    iot_rf_power_policy_config_t config = {
        .levels_dbm = k_levels,
        .level_count = (uint8_t)(sizeof(k_levels) / sizeof(k_levels[0])),
        .min_power_dbm = -10,
        .max_power_dbm = 20,
        .lqi_good_threshold = 240,
        .good_windows_needed = 3,
        .samples_needed = 6,
    };
    return config;
}

static void test_config_and_helpers(void)
{
    iot_rf_power_policy_config_t config = make_config();
    iot_rf_power_policy_config_t invalid;
    uint8_t index = 0xFF;

    assert(iot_rf_power_policy_config_valid(&config));
    assert(iot_rf_power_policy_nearest_index(&config, 20, &index));
    assert(index == 5);
    assert(iot_rf_power_policy_nearest_index(&config, 5, &index));
    assert(index == 2); /* 8 dBm is closer than 0 dBm */
    assert(iot_rf_power_policy_value_allowed(&config, -10));
    assert(iot_rf_power_policy_value_allowed(&config, 20));
    assert(!iot_rf_power_policy_value_allowed(&config, 5));
    assert(!iot_rf_power_policy_nearest_index(NULL, 0, &index));

    invalid = config;
    invalid.levels_dbm = NULL;
    assert(!iot_rf_power_policy_config_valid(&invalid));

    invalid = config;
    invalid.level_count = 1;
    assert(!iot_rf_power_policy_config_valid(&invalid));

    invalid = config;
    invalid.good_windows_needed = 0;
    assert(!iot_rf_power_policy_config_valid(&invalid));

    invalid = config;
    invalid.min_power_dbm = 20;
    invalid.max_power_dbm = -10;
    assert(!iot_rf_power_policy_config_valid(&invalid));

    invalid = config;
    invalid.min_power_dbm = 5;
    assert(!iot_rf_power_policy_config_valid(&invalid));
}

static void test_insufficient_samples_keeps_state(void)
{
    iot_rf_power_policy_config_t config = make_config();
    iot_rf_power_policy_input_t input = {
        .samples = 5,
        .lqi_ema = 250,
        .current_power_dbm = 20,
        .good_windows = 2,
    };
    iot_rf_power_policy_decision_t decision;

    assert(iot_rf_power_policy_decide_auto(&config, &input, &decision));
    assert(!decision.enough_samples);
    assert(decision.current_in_bounds);
    assert(decision.action == IOT_RF_POWER_ACTION_NONE);
    assert(decision.good_windows == 2);
    assert(decision.target_power_dbm == 20);
}

static void test_out_of_bounds_keeps_state(void)
{
    iot_rf_power_policy_config_t config = make_config();
    iot_rf_power_policy_input_t input = {
        .samples = 6,
        .lqi_ema = 250,
        .current_power_dbm = 20,
        .good_windows = 2,
    };
    iot_rf_power_policy_decision_t decision;

    config.min_power_dbm = 0;
    config.max_power_dbm = 18;
    assert(iot_rf_power_policy_decide_auto(&config, &input, &decision));
    assert(decision.enough_samples);
    assert(!decision.current_in_bounds);
    assert(decision.action == IOT_RF_POWER_ACTION_NONE);
    assert(decision.good_windows == 2);
}

static void test_bad_lqi_resets_windows(void)
{
    iot_rf_power_policy_config_t config = make_config();
    iot_rf_power_policy_input_t input = {
        .samples = 6,
        .lqi_ema = 240, /* threshold is strictly greater than */
        .current_power_dbm = 18,
        .good_windows = 2,
    };
    iot_rf_power_policy_decision_t decision;

    assert(iot_rf_power_policy_decide_auto(&config, &input, &decision));
    assert(decision.action == IOT_RF_POWER_ACTION_NONE);
    assert(decision.good_windows == 0);
}

static void test_good_window_hysteresis_and_power_down(void)
{
    iot_rf_power_policy_config_t config = make_config();
    iot_rf_power_policy_input_t input = {
        .samples = 6,
        .lqi_ema = 241,
        .current_power_dbm = 20,
        .good_windows = 0,
    };
    iot_rf_power_policy_decision_t decision;
    int round;

    for (round = 1; round <= 2; ++round) {
        assert(iot_rf_power_policy_decide_auto(&config, &input, &decision));
        assert(decision.action == IOT_RF_POWER_ACTION_NONE);
        assert(decision.good_windows == (uint8_t)round);
        input.good_windows = decision.good_windows;
    }

    assert(iot_rf_power_policy_decide_auto(&config, &input, &decision));
    assert(decision.action == IOT_RF_POWER_ACTION_SET);
    assert(decision.target_power_dbm == 18);
    assert(decision.good_windows == 0);
}

static void test_does_not_reduce_below_minimum(void)
{
    iot_rf_power_policy_config_t config = make_config();
    iot_rf_power_policy_input_t input = {
        .samples = 6,
        .lqi_ema = 255,
        .current_power_dbm = -10,
        .good_windows = 2,
    };
    iot_rf_power_policy_decision_t decision;

    assert(iot_rf_power_policy_decide_auto(&config, &input, &decision));
    assert(decision.action == IOT_RF_POWER_ACTION_NONE);
    assert(decision.target_power_dbm == -10);
    assert(decision.good_windows == 3);
}

static void test_good_window_saturates(void)
{
    iot_rf_power_policy_config_t config = make_config();
    iot_rf_power_policy_input_t input = {
        .samples = 6,
        .lqi_ema = 255,
        .current_power_dbm = -10,
        .good_windows = UINT8_MAX - 1U,
    };
    iot_rf_power_policy_decision_t decision;

    assert(iot_rf_power_policy_decide_auto(&config, &input, &decision));
    assert(decision.good_windows == UINT8_MAX);
    assert(decision.action == IOT_RF_POWER_ACTION_NONE);
}

static void test_invalid_arguments(void)
{
    iot_rf_power_policy_config_t config = make_config();
    iot_rf_power_policy_input_t input = {
        .samples = 6,
        .lqi_ema = 255,
        .current_power_dbm = 20,
    };
    iot_rf_power_policy_decision_t decision;

    assert(!iot_rf_power_policy_decide_auto(NULL, &input, &decision));
    assert(!iot_rf_power_policy_decide_auto(&config, NULL, &decision));
    assert(!iot_rf_power_policy_decide_auto(&config, &input, NULL));
}

int main(void)
{
    test_config_and_helpers();
    test_insufficient_samples_keeps_state();
    test_out_of_bounds_keeps_state();
    test_bad_lqi_resets_windows();
    test_good_window_hysteresis_and_power_down();
    test_does_not_reduce_below_minimum();
    test_good_window_saturates();
    test_invalid_arguments();
    puts("iot_rf_power_policy host tests passed");
    return 0;
}
