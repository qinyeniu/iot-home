// CQ-4b: host-testable pre-send plan for periodic background RF commands.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rf_command_queue.h"

#define RF_BG_DISPATCH_POWER_UNKNOWN_DBM (-128)
#define RF_BG_DISPATCH_MODE_UNKNOWN 0xffu

typedef enum {
    RF_BG_DISPATCH_SEND = 0,
    RF_BG_DISPATCH_CLOSE_EXPIRED,
    RF_BG_DISPATCH_CLOSE_STATE_CHANGED,
    RF_BG_DISPATCH_CLOSE_INACTIVE,
    RF_BG_DISPATCH_DROP_STALE,
    RF_BG_DISPATCH_INVALID_ARG,
} rf_background_dispatch_status_t;

typedef struct {
    uint32_t now_ms;
    uint16_t node_addr;
    uint8_t packet_sequence;
    uint32_t active_command_id;
    uint32_t active_command_generation;
    bool user_intent_active;
    bool report_waiting;
    bool report_submitting;
    bool dispatch_active;
} rf_background_dispatch_state_t;

typedef struct {
    rf_background_dispatch_status_t status;
    uint16_t node_addr;
    uint8_t seq;
    uint8_t command;
    int8_t packet_power_dbm;
    uint8_t packet_mode;
    int8_t expected_report_power_dbm;
    uint8_t expected_report_mode;
} rf_background_dispatch_plan_t;

/*
 * Decides how one dequeued periodic background command should be handled. The
 * caller must snapshot state while holding the RF lock and apply any CLOSE_*
 * transition under that lock. A DROP_STALE command is no longer the owner and
 * must not mutate current background state.
 *
 * This module does not call FreeRTOS, ZBOSS, NVS, MQTT or network APIs.
 */
bool rf_background_dispatch_plan_create(
    const rf_dispatch_cmd_t *command,
    const rf_background_dispatch_state_t *state,
    rf_background_dispatch_plan_t *plan);
