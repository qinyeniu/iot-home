// CQ-3: host-testable admission decision for background RF work.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RF_BACKGROUND_EVAL_MS 60000u

typedef enum {
    RF_BACKGROUND_ADMISSION_ADMIT = 0,
    RF_BACKGROUND_REJECT_EVAL_COOLDOWN,
    RF_BACKGROUND_REJECT_TX_BACKOFF,
    RF_BACKGROUND_REJECT_NODE_INACTIVE,
    RF_BACKGROUND_REJECT_USER_PRIORITY,
    RF_BACKGROUND_REJECT_REPORT_WAITING,
    RF_BACKGROUND_REJECT_DISPATCH_ACTIVE,
    RF_BACKGROUND_REJECT_BACKGROUND_ACTIVE,
} rf_background_admission_result_t;

typedef struct {
    uint32_t now_ms;
    uint32_t last_eval_ms;
    uint16_t node_addr;
    bool tx_backoff_active;
    bool user_intent_active;
    bool report_waiting;
    bool report_submitting;
    bool dispatch_active;
    bool background_command_active;
} rf_background_admission_state_t;

/*
 * Decides whether one background RF evaluation may start. The caller must read
 * all fields while holding the RF state lock. When the result is ADMIT, the
 * caller must still inside that same lock set last_eval_ms to state.now_ms and
 * mark the background command active before releasing the lock; otherwise a
 * second evaluation could be admitted.
 *
 * Timestamps use one monotonic millisecond clock. The normal 60 second cadence
 * is represented by unsigned modular subtraction, so the nearby uint32 wrap is
 * handled without assuming int32 signed differences.
 */
bool rf_background_admission_decide(
    const rf_background_admission_state_t *state,
    rf_background_admission_result_t *result);
