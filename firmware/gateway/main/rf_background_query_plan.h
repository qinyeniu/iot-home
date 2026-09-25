// CQ-3b: host-testable background QUERY plan.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RF_BG_NODE_FRESH_MS 120000u
#define RF_BG_WAIT_SETTLE_MS 90000u
#define RF_BG_RECONCILE_MS 300000u
#define RF_BG_POWER_UNKNOWN_DBM (-128)

typedef enum {
    RF_BG_QUERY_PLAN_SEND = 0,
    RF_BG_QUERY_PLAN_NO_SEND,
    RF_BG_QUERY_PLAN_NO_SEND_WAITING,
    RF_BG_QUERY_PLAN_NODE_STALE,
    RF_BG_QUERY_PLAN_INVALID_ARG,
} rf_background_query_plan_result_t;

typedef struct {
    uint32_t now_ms;
    uint16_t node_addr;
    uint32_t last_rx_ms;
    uint32_t confirmed_ms;
    uint32_t wait_started_ms;
    int8_t current_power_dbm;
} rf_background_query_plan_state_t;

typedef struct {
    rf_background_query_plan_result_t result;
} rf_background_query_plan_t;

/*
 * Creates a plan for one already-admitted background evaluation. The caller is
 * responsible for admission checks and, while still holding the RF state lock,
 * applying any returned state transition. If result is NODE_STALE, the caller
 * should clear stale RF node state in that same lock and wait for the next
 * evaluation rather than sending immediately.
 *
 * Unsigned modular elapsed-time comparisons are safe at the uint32 tick wrap.
 */
bool rf_background_query_plan_create(
    const rf_background_query_plan_state_t *state,
    rf_background_query_plan_t *plan);
