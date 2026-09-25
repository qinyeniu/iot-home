// CQ-4a: host-testable mapper from gateway RF raw state to CQ runtime state.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rf_background_runtime_adapter.h"

// These values intentionally mirror the gateway RF_MODE_* contract before the
// mapper translates it into the CQ module vocabulary.
#define RF_BG_MAPPER_MODE_AUTO 0u
#define RF_BG_MAPPER_MODE_MANUAL 1u
#define RF_BG_MAPPER_MODE_UNKNOWN 0xffu
#define RF_BG_MAPPER_POWER_UNKNOWN_DBM (-128)

typedef enum {
    RF_BG_MAPPER_OK = 0,
    RF_BG_MAPPER_INVALID_ARG,
    RF_BG_MAPPER_INVALID_POWER,
    RF_BG_MAPPER_INVALID_MODE,
    RF_BG_MAPPER_INVALID_STATE,
} rf_background_command_mapper_result_t;

typedef struct {
    uint32_t now_ms;
    uint32_t last_eval_ms;
    uint16_t node_addr;
    uint32_t last_rx_ms;
    uint32_t confirmed_ms;
    int8_t current_power_dbm;
    uint8_t node_mode;
    uint8_t desired_mode;
    int8_t desired_manual_power_dbm;
    bool policy_apply_pending;
    uint32_t tx_backoff_until_ms;
    bool user_intent_active;
    bool report_waiting;
    bool report_submitting;
    uint32_t wait_started_ms;
    bool dispatch_active;
    bool background_command_active;
    uint32_t active_command_id;
    uint32_t active_command_generation;
    uint32_t next_command_id;
    uint32_t policy_generation;
} rf_background_command_mapper_input_t;

typedef struct {
    rf_background_command_mapper_result_t result;
    rf_background_runtime_state_t runtime;
} rf_background_command_mapper_output_t;

/*
 * Validates and maps a snapshot of the gateway raw RF state into the CQ-3e
 * runtime state. The caller must hold the RF state lock while filling the input
 * snapshot. This function does not call FreeRTOS, ZBOSS, NVS, MQTT or network
 * APIs and does not mutate gateway state.
 *
 * A successful mapping only produces a runtime snapshot; the caller still must
 * explicitly decide whether to apply it or submit a background command.
 */
bool rf_background_command_mapper_map(
    const rf_background_command_mapper_input_t *input,
    rf_background_command_mapper_output_t *output);
