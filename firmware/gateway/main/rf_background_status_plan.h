// CQ-3f: host-testable status plan for background RF command outcomes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rf_command_queue.h"

typedef enum {
    RF_BG_STATUS_EVENT_MATCHED_REPORT = 0,
    RF_BG_STATUS_EVENT_WAIT_TIMEOUT,
    RF_BG_STATUS_EVENT_SEND_FAILED,
} rf_background_status_event_t;

typedef enum {
    RF_BG_STATUS_PUBLISH_REPORT_RECEIVED = 0,
    RF_BG_STATUS_PUBLISH_CONFIRMED,
    RF_BG_STATUS_PUBLISH_TIMEOUT,
    RF_BG_STATUS_PUBLISH_FAILED,
} rf_background_publish_status_t;

typedef enum {
    RF_BG_COMMAND_TABLE_NONE = 0,
    RF_BG_COMMAND_TABLE_ACKNOWLEDGED,
    RF_BG_COMMAND_TABLE_TIMEOUT,
    RF_BG_COMMAND_TABLE_FAILED,
} rf_background_command_table_action_t;

typedef enum {
    RF_BG_STATUS_PLAN_PUBLISH = 0,
    RF_BG_STATUS_PLAN_INTERNAL_CLOSE,
    RF_BG_STATUS_PLAN_NO_ACTION,
    RF_BG_STATUS_PLAN_INVALID_ARG,
} rf_background_status_plan_result_t;

typedef struct {
    uint32_t now_ms;
    rf_dispatch_cmd_t command;
    rf_background_status_event_t event;
    int8_t reported_power_dbm;
    uint8_t reported_mode;
    bool matched_transaction;
    bool node_initiated_report;
    bool policy_apply_pending;
    // Throttles retained negative status notices. A timeout/failure still
    // closes the active command even when this is false.
    bool notice_allowed;
} rf_background_status_state_t;

typedef struct {
    rf_background_status_plan_result_t result;
    rf_background_publish_status_t publish_status;
    rf_background_command_table_action_t table_action;
    int8_t reported_power_dbm;
    uint8_t reported_mode;
    bool clear_background;
    bool set_policy_pending;
    bool policy_pending_value;
} rf_background_status_plan_t;

/*
 * Decides how one active periodic background command should be completed. The
 * caller must snapshot state and the active command while holding the RF lock.
 * This module does not call FreeRTOS, ZBOSS, NVS, MQTT or network APIs.
 *
 * A matched QUERY produces report_received. A satisfied SET produces
 * confirmed. Timeout/send failure marks the command terminal; if negative
 * notice publication is throttled, the command still closes internally. A
 * failed SET explicitly leaves policy pending for the next convergence; a
 * failed QUERY leaves policy_apply_pending untouched.
 *
 * set_policy_pending=false means "do not change policy_apply_pending", not
 * "set it to false". The caller remains responsible for MQTT publication and
 * for applying state transitions under the RF lock.
 */
bool rf_background_status_plan_create(
    const rf_background_status_state_t *state,
    rf_background_status_plan_t *plan);
