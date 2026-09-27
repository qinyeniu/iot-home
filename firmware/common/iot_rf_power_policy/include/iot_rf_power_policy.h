#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Actions selected by the automatic RF power policy. */
typedef enum {
    IOT_RF_POWER_ACTION_NONE = 0,
    IOT_RF_POWER_ACTION_SET = 1,
} iot_rf_power_action_t;

/**
 * Board and policy configuration.
 *
 * levels_dbm must point to a strictly increasing table. min/max power must
 * exist in that table and define the range allowed for automatic control.
 */
typedef struct {
    const int8_t *levels_dbm;
    uint8_t level_count;
    int8_t min_power_dbm;
    int8_t max_power_dbm;

    /** LQI strictly above this value counts as one good evaluation window. */
    int lqi_good_threshold;

    /** Number of consecutive good windows required to reduce power. */
    uint8_t good_windows_needed;

    /** Number of link samples required before evaluating automatic power. */
    uint8_t samples_needed;
} iot_rf_power_policy_config_t;

/** Runtime values consumed by one automatic-power evaluation. */
typedef struct {
    uint8_t samples;
    int lqi_ema;
    int8_t current_power_dbm;

    /** Good-window counter carried from the previous evaluation. */
    uint8_t good_windows;
} iot_rf_power_policy_input_t;

/** Result of one automatic-power evaluation. */
typedef struct {
    iot_rf_power_action_t action;
    int8_t target_power_dbm;
    uint8_t current_index;

    /** Counter that the caller should persist for the next evaluation. */
    uint8_t good_windows;

    bool enough_samples;
    bool current_in_bounds;
} iot_rf_power_policy_decision_t;

/** Validate the power table, bounds and hysteresis configuration. */
bool iot_rf_power_policy_config_valid(
    const iot_rf_power_policy_config_t *config);

/** Find the nearest level index; returns false for invalid arguments. */
bool iot_rf_power_policy_nearest_index(
    const iot_rf_power_policy_config_t *config,
    int8_t power_dbm,
    uint8_t *index);

/** Return true when power is a table value inside the configured min/max. */
bool iot_rf_power_policy_value_allowed(
    const iot_rf_power_policy_config_t *config,
    int8_t power_dbm);

/**
 * Decide one automatic-power evaluation.
 *
 * The function is pure: it does not access hardware or global state. The
 * caller persists decision.good_windows and performs the requested SET.
 */
bool iot_rf_power_policy_decide_auto(
    const iot_rf_power_policy_config_t *config,
    const iot_rf_power_policy_input_t *input,
    iot_rf_power_policy_decision_t *decision);

#ifdef __cplusplus
}
#endif
