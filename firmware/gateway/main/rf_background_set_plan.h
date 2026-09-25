// CQ-3c: host-testable background SET plan for desired MANUAL/AUTO policy.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rf_command_queue.h"

#define RF_BG_SET_MODE_UNKNOWN 0xffu
#define RF_BG_SET_POWER_UNKNOWN_DBM (-128)

typedef enum {
    RF_BG_SET_PLAN_SEND = 0,
    RF_BG_SET_PLAN_NO_SEND,
    RF_BG_SET_PLAN_CLEAR_PENDING,
    RF_BG_SET_PLAN_INVALID_ARG,
} rf_background_set_plan_result_t;

typedef struct {
    uint32_t now_ms;
    uint16_t node_addr;
    int8_t current_power_dbm;
    uint8_t node_mode;
    uint8_t desired_mode;
    int8_t desired_manual_power_dbm;
    bool policy_apply_pending;
} rf_background_set_plan_state_t;

typedef struct {
    rf_background_set_plan_result_t result;
    int8_t set_power_dbm;
    uint8_t set_mode;
    bool mark_policy_pending;
} rf_background_set_plan_t;

/*
 * Creates a SET plan for one already-admitted background evaluation. This is
 * not a standalone API: the caller must first apply background admission and
 * the QUERY-plan freshness gate, and must not invoke it while a non-expired
 * report transaction is waiting.
 *
 * If result is SEND and mark_policy_pending is true, the caller must still
 * inside the RF state lock mark policy apply pending, reserve the private
 * packet sequence and install waiting before releasing the lock. If the later
 * ZBOSS request fails, the caller shall roll back that preinstalled waiting
 * state while preserving policy_apply_pending for the next evaluation.
 *
 * CLEAR_PENDING means the node already satisfies the desired policy according
 * to the fresh QUERY/REPORT evidence; the caller shall clear
 * policy_apply_pending in that same RF lock. An unknown node mode is not
 * changed here; an earlier/later QUERY must establish it.
 */
bool rf_background_set_plan_create(
    const rf_background_set_plan_state_t *state,
    rf_background_set_plan_t *plan);
