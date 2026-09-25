/*
 * Low-wear Flash spool flush policy.
 *
 * Pure C and independent of ESP-IDF so host tests can verify when cached data
 * is allowed to move from RAM to Flash. This module performs no I/O.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_SPOOL_MIN_FLUSH_INTERVAL_MS 60000U
#define IOT_SPOOL_FLUSH_BATCH_RECORDS    16U
#define IOT_SPOOL_FLUSH_MAX_AGE_MS       120000U
#define IOT_SPOOL_MAX_FLUSHES_PER_HOUR   10U
#define IOT_SPOOL_MAX_FLUSHES_PER_DAY    20U

typedef enum {
    IOT_SPOOL_REASON_NONE = 0,
    IOT_SPOOL_REASON_BATCH_READY,
    IOT_SPOOL_REASON_MAX_AGE,
    IOT_SPOOL_REASON_FORCED,
    IOT_SPOOL_REASON_NO_DIRTY_DATA,
    IOT_SPOOL_REASON_BATCH_OR_AGE_WAIT,
    IOT_SPOOL_REASON_MIN_INTERVAL_WAIT,
    IOT_SPOOL_REASON_HOUR_BUDGET,
    IOT_SPOOL_REASON_DAY_BUDGET,
    IOT_SPOOL_REASON_INVALID_ARG,
} iot_spool_reason_t;

typedef struct {
    uint32_t last_flush_ms;
    uint32_t window_hour_start_ms;
    uint32_t window_day_start_ms;
    uint16_t flushes_in_hour;
    uint16_t flushes_in_day;
    bool have_last_flush;
} iot_spool_policy_state_t;

/*
 * Usage contract for future Flash integration:
 * 1. Zero-initialize state; do not modify counters outside this component.
 * 2. All timestamps must use the same monotonic uptime millisecond clock.
 * 3. One writer must serialize should_flush -> Flash transaction -> on_flush so two
 *    tasks cannot consume the same remaining write budget.
 */

/**
 * Decide whether cached RAM records may be written as one Flash batch.
 *
 * This function is read-only; call on_flush only after the approved Flash write
 * has completed successfully.
 *
 * @param dirty_count Number of unflushed records.
 * @param oldest_dirty_ms Timestamp of the oldest unflushed record.
 * @param forced If true, bypass batch/age/min-interval, but not write budgets.
 * @param[out] reason Machine-readable decision reason; may be NULL.
 * @return true only when one bounded Flash write is allowed.
 */
bool iot_spool_policy_should_flush(const iot_spool_policy_state_t *state,
                                  uint32_t now_ms,
                                  size_t dirty_count,
                                  uint32_t oldest_dirty_ms,
                                  bool forced,
                                  iot_spool_reason_t *reason);

/** Record one successful Flash write and rotate hourly/daily budget windows. */
void iot_spool_policy_on_flush(iot_spool_policy_state_t *state,
                               uint32_t now_ms);

#ifdef __cplusplus
}
#endif
