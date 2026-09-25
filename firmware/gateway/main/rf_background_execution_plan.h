// CQ-3d: composed background RF execution plan.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rf_background_query_plan.h"
#include "rf_background_schedule.h"
#include "rf_background_set_plan.h"
#include "rf_command_queue.h"

typedef enum {
    RF_BG_EXEC_IDLE = 0,
    RF_BG_EXEC_READY_QUERY,
    RF_BG_EXEC_READY_SET,
    RF_BG_EXEC_POLICY_ALREADY_APPLIED,
    RF_BG_EXEC_NODE_STALE,
    RF_BG_EXEC_REJECTED,
    RF_BG_EXEC_INVALID_ARG,
} rf_background_execution_result_t;

typedef struct {
    uint32_t now_ms;
    uint32_t last_eval_ms;
    uint16_t node_addr;
    uint32_t last_rx_ms;
    uint32_t confirmed_ms;
    uint32_t wait_started_ms;
    int8_t current_power_dbm;
    uint8_t node_mode;
    uint8_t desired_mode;
    int8_t desired_manual_power_dbm;
    bool policy_apply_pending;
    bool tx_backoff_active;
    bool user_intent_active;
    bool report_waiting;
    bool report_submitting;
    bool dispatch_active;
    bool background_command_active;
    uint32_t next_command_id;
    uint32_t policy_generation;
} rf_background_execution_state_t;

typedef struct {
    rf_background_execution_result_t result;
    rf_background_admission_result_t reject_reason;
    rf_dispatch_cmd_t command;
    bool update_last_eval;
    bool background_active;
    bool clear_stale_state;
    bool clear_existing_waiting;
    bool clear_policy_pending;
    bool mark_policy_pending;
} rf_background_execution_plan_t;

/*
 * Combines background admission, QUERY planning and SET planning in one pure
 * evaluation. The caller must snapshot all fields while holding the RF state
 * lock. The function does not call FreeRTOS, ZBOSS, NVS or network APIs.
 *
 * A non-expired waiting transaction is rejected. Once a waiting transaction is
 * due, the old waiting/background ownership is cleared after admission and a
 * QUERY is produced. A stale node clears runtime RF, waiting and background
 * state while retaining the node address and durable desired policy;
 * policy_apply_pending is marked for the next convergence.
 *
 * For READY results, the caller must still inside the same lock apply the
 * boolean state changes, copy the queue command, release the lock, and then
 * submit that command to the single RF queue rather than sending directly.
 *
 * If queue submission fails, the caller must re-enter the RF lock, verify the
 * same command id and policy generation, clear only that background ownership,
 * and preserve policy_apply_pending. No packet sequence or report waiting may
 * be installed for a command that did not enter the queue.
 */
bool rf_background_execution_plan_create(
    const rf_background_execution_state_t *state,
    rf_background_execution_plan_t *plan);
