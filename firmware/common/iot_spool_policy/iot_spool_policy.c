/*
 * Low-wear Flash spool flush policy.
 */

#include "iot_spool_policy.h"

#define HOUR_MS 3600000U
#define DAY_MS  86400000U

static uint32_t elapsed_ms(uint32_t now_ms, uint32_t past_ms)
{
    return now_ms - past_ms;
}

static void rotate_windows(iot_spool_policy_state_t *state, uint32_t now_ms)
{
    if (elapsed_ms(now_ms, state->window_hour_start_ms) >= HOUR_MS) {
        state->window_hour_start_ms = now_ms;
        state->flushes_in_hour = 0U;
    }
    if (elapsed_ms(now_ms, state->window_day_start_ms) >= DAY_MS) {
        state->window_day_start_ms = now_ms;
        state->flushes_in_day = 0U;
    }
}

static uint16_t saturating_increment(uint16_t value)
{
    return value == UINT16_MAX ? value : (uint16_t)(value + 1U);
}

bool iot_spool_policy_should_flush(const iot_spool_policy_state_t *state,
                                  uint32_t now_ms,
                                  size_t dirty_count,
                                  uint32_t oldest_dirty_ms,
                                  bool forced,
                                  iot_spool_reason_t *reason)
{
    iot_spool_policy_state_t mutable_state;
    uint32_t age_ms;
    bool batch_ready;
    bool age_ready;

    if (reason != NULL) {
        *reason = IOT_SPOOL_REASON_NONE;
    }
    if (state == NULL) {
        if (reason != NULL) {
            *reason = IOT_SPOOL_REASON_INVALID_ARG;
        }
        return false;
    }

    mutable_state = *state;
    rotate_windows(&mutable_state, now_ms);

    if (mutable_state.flushes_in_hour >= IOT_SPOOL_MAX_FLUSHES_PER_HOUR) {
        if (reason != NULL) {
            *reason = IOT_SPOOL_REASON_HOUR_BUDGET;
        }
        return false;
    }
    if (mutable_state.flushes_in_day >= IOT_SPOOL_MAX_FLUSHES_PER_DAY) {
        if (reason != NULL) {
            *reason = IOT_SPOOL_REASON_DAY_BUDGET;
        }
        return false;
    }
    if (dirty_count == 0U) {
        if (reason != NULL) {
            *reason = IOT_SPOOL_REASON_NO_DIRTY_DATA;
        }
        return false;
    }

    age_ms = elapsed_ms(now_ms, oldest_dirty_ms);
    batch_ready = dirty_count >= IOT_SPOOL_FLUSH_BATCH_RECORDS;
    age_ready = age_ms >= IOT_SPOOL_FLUSH_MAX_AGE_MS;

    if (!forced && !batch_ready && !age_ready) {
        if (reason != NULL) {
            *reason = IOT_SPOOL_REASON_BATCH_OR_AGE_WAIT;
        }
        return false;
    }
    if (!forced && mutable_state.have_last_flush &&
        elapsed_ms(now_ms, mutable_state.last_flush_ms) <
            IOT_SPOOL_MIN_FLUSH_INTERVAL_MS) {
        if (reason != NULL) {
            *reason = IOT_SPOOL_REASON_MIN_INTERVAL_WAIT;
        }
        return false;
    }

    if (reason != NULL) {
        if (forced) {
            *reason = IOT_SPOOL_REASON_FORCED;
        } else {
            *reason = batch_ready ? IOT_SPOOL_REASON_BATCH_READY
                                 : IOT_SPOOL_REASON_MAX_AGE;
        }
    }
    return true;
}

void iot_spool_policy_on_flush(iot_spool_policy_state_t *state,
                               uint32_t now_ms)
{
    if (state == NULL) {
        return;
    }

    rotate_windows(state, now_ms);
    state->last_flush_ms = now_ms;
    state->have_last_flush = true;
    state->flushes_in_hour =
        saturating_increment(state->flushes_in_hour);
    state->flushes_in_day =
        saturating_increment(state->flushes_in_day);
}
