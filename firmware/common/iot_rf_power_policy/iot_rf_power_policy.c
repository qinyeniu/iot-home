#include "iot_rf_power_policy.h"

#include <stddef.h>

static bool nearest_index(const iot_rf_power_policy_config_t *config,
                          int8_t power_dbm,
                          uint8_t *index)
{
    uint8_t best = 0;
    uint16_t best_delta = 0xFFFFU;
    uint8_t i;

    if (config == NULL || config->levels_dbm == NULL || index == NULL ||
        config->level_count == 0U) {
        return false;
    }

    for (i = 0; i < config->level_count; ++i) {
        int delta = (int)power_dbm - (int)config->levels_dbm[i];
        uint16_t abs_delta = (uint16_t)(delta < 0 ? -delta : delta);
        if (abs_delta < best_delta) {
            best_delta = abs_delta;
            best = i;
        }
    }

    *index = best;
    return true;
}

bool iot_rf_power_policy_config_valid(
    const iot_rf_power_policy_config_t *config)
{
    uint8_t min_index;
    uint8_t max_index;
    uint8_t i;

    if (config == NULL || config->levels_dbm == NULL ||
        config->level_count < 2U ||
        config->good_windows_needed == 0U ||
        config->samples_needed == 0U) {
        return false;
    }

    for (i = 1U; i < config->level_count; ++i) {
        if (config->levels_dbm[i] <= config->levels_dbm[i - 1U]) {
            return false;
        }
    }

    if (!nearest_index(config, config->min_power_dbm, &min_index) ||
        !nearest_index(config, config->max_power_dbm, &max_index) ||
        config->levels_dbm[min_index] != config->min_power_dbm ||
        config->levels_dbm[max_index] != config->max_power_dbm ||
        min_index > max_index) {
        return false;
    }

    return true;
}

bool iot_rf_power_policy_nearest_index(
    const iot_rf_power_policy_config_t *config,
    int8_t power_dbm,
    uint8_t *index)
{
    if (!iot_rf_power_policy_config_valid(config)) {
        return false;
    }
    return nearest_index(config, power_dbm, index);
}

bool iot_rf_power_policy_value_allowed(
    const iot_rf_power_policy_config_t *config,
    int8_t power_dbm)
{
    uint8_t index;

    if (!iot_rf_power_policy_config_valid(config) ||
        !nearest_index(config, power_dbm, &index)) {
        return false;
    }

    return config->levels_dbm[index] == power_dbm &&
           power_dbm >= config->min_power_dbm &&
           power_dbm <= config->max_power_dbm;
}

bool iot_rf_power_policy_decide_auto(
    const iot_rf_power_policy_config_t *config,
    const iot_rf_power_policy_input_t *input,
    iot_rf_power_policy_decision_t *decision)
{
    uint8_t min_index;
    uint8_t max_index;
    uint8_t current_index;
    uint8_t good_windows;

    if (!iot_rf_power_policy_config_valid(config) || input == NULL ||
        decision == NULL) {
        return false;
    }

    if (!nearest_index(config, config->min_power_dbm, &min_index) ||
        !nearest_index(config, config->max_power_dbm, &max_index) ||
        !nearest_index(config, input->current_power_dbm, &current_index)) {
        return false;
    }

    good_windows = input->good_windows;
    decision->action = IOT_RF_POWER_ACTION_NONE;
    decision->target_power_dbm = input->current_power_dbm;
    decision->current_index = current_index;
    decision->good_windows = good_windows;
    decision->enough_samples =
        input->samples >= config->samples_needed;
    decision->current_in_bounds =
        current_index >= min_index && current_index <= max_index;

    if (!decision->enough_samples || !decision->current_in_bounds) {
        return true;
    }

    if (input->lqi_ema > config->lqi_good_threshold) {
        if (good_windows < UINT8_MAX) {
            ++good_windows;
        }
    } else {
        good_windows = 0U;
    }

    decision->good_windows = good_windows;
    if (good_windows >= config->good_windows_needed &&
        current_index > min_index) {
        decision->action = IOT_RF_POWER_ACTION_SET;
        decision->target_power_dbm =
            config->levels_dbm[current_index - 1U];
        decision->good_windows = 0U;
    }

    return true;
}
