#include "iot_spool_policy.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(condition) do {                                          \
    if (!(condition)) {                                                \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,     \
               __LINE__);                                              \
        ++s_failures;                                                  \
    }                                                                  \
} while (0)

static iot_spool_policy_state_t fresh_state(void)
{
    iot_spool_policy_state_t state;

    memset(&state, 0, sizeof(state));
    return state;
}

static void test_invalid_and_empty(void)
{
    iot_spool_policy_state_t state = fresh_state();
    iot_spool_reason_t reason = IOT_SPOOL_REASON_NONE;

    CHECK(!iot_spool_policy_should_flush(NULL, 100U, 1U, 0U, false,
                                         &reason));
    CHECK(reason == IOT_SPOOL_REASON_INVALID_ARG);

    CHECK(!iot_spool_policy_should_flush(&state, 100U, 0U, 0U, false,
                                         &reason));
    CHECK(reason == IOT_SPOOL_REASON_NO_DIRTY_DATA);

    CHECK(!iot_spool_policy_should_flush(&state, 100000U, 8U, 95000U,
                                         false, &reason));
    CHECK(reason == IOT_SPOOL_REASON_BATCH_OR_AGE_WAIT);

    iot_spool_policy_on_flush(NULL, 100U);
}

static void test_batch_and_age_flush(void)
{
    iot_spool_policy_state_t state = fresh_state();
    iot_spool_reason_t reason = IOT_SPOOL_REASON_NONE;

    CHECK(iot_spool_policy_should_flush(&state, 1000U, 16U, 0U, false,
                                        &reason));
    CHECK(reason == IOT_SPOOL_REASON_BATCH_READY);

    state = fresh_state();
    CHECK(iot_spool_policy_should_flush(&state, 120000U, 1U, 0U, false,
                                        &reason));
    CHECK(reason == IOT_SPOOL_REASON_MAX_AGE);

    /* Querying must not mutate state; counters remain zero. */
    CHECK(state.flushes_in_hour == 0U);
    CHECK(state.flushes_in_day == 0U);
    CHECK(!state.have_last_flush);
}

static void test_min_interval_and_forced(void)
{
    iot_spool_policy_state_t state = fresh_state();
    iot_spool_reason_t reason = IOT_SPOOL_REASON_NONE;

    iot_spool_policy_on_flush(&state, 0U);
    CHECK(state.flushes_in_hour == 1U);
    CHECK(state.flushes_in_day == 1U);
    CHECK(state.have_last_flush);

    CHECK(!iot_spool_policy_should_flush(&state, 30000U, 16U, 0U, false,
                                         &reason));
    CHECK(reason == IOT_SPOOL_REASON_MIN_INTERVAL_WAIT);

    CHECK(iot_spool_policy_should_flush(&state, 30000U, 1U, 30000U, true,
                                        &reason));
    CHECK(reason == IOT_SPOOL_REASON_FORCED);

    CHECK(iot_spool_policy_should_flush(&state, 60000U, 16U, 0U, false,
                                        &reason));
    CHECK(reason == IOT_SPOOL_REASON_BATCH_READY);
}

static void test_budgets(void)
{
    iot_spool_policy_state_t state = fresh_state();
    iot_spool_reason_t reason = IOT_SPOOL_REASON_NONE;

    for (uint16_t i = 0; i < IOT_SPOOL_MAX_FLUSHES_PER_HOUR; ++i) {
        iot_spool_policy_on_flush(&state, (uint32_t)i);
    }
    CHECK(!iot_spool_policy_should_flush(&state, 100U, 16U, 0U, false,
                                         &reason));
    CHECK(reason == IOT_SPOOL_REASON_HOUR_BUDGET);

    /* Forced operation is still bounded by the wear budgets. */
    CHECK(!iot_spool_policy_should_flush(&state, 100U, 1U, 100U, true,
                                         &reason));
    CHECK(reason == IOT_SPOOL_REASON_HOUR_BUDGET);

    state = fresh_state();
    state.flushes_in_day = IOT_SPOOL_MAX_FLUSHES_PER_DAY;
    CHECK(!iot_spool_policy_should_flush(&state, 100U, 16U, 0U, false,
                                         &reason));
    CHECK(reason == IOT_SPOOL_REASON_DAY_BUDGET);
    CHECK(!iot_spool_policy_should_flush(&state, 100U, 1U, 100U, true,
                                         &reason));
    CHECK(reason == IOT_SPOOL_REASON_DAY_BUDGET);
}

static void test_window_rotation_lifecycle(void)
{
    const uint32_t old_time = 0xffffff00U;
    const uint32_t hour_now = old_time + 3600000U;
    iot_spool_policy_state_t state = fresh_state();
    iot_spool_reason_t reason = IOT_SPOOL_REASON_NONE;

    state.window_hour_start_ms = old_time;
    state.window_day_start_ms = old_time;
    state.flushes_in_hour = IOT_SPOOL_MAX_FLUSHES_PER_HOUR;
    state.flushes_in_day = 1U;

    CHECK(iot_spool_policy_should_flush(&state, hour_now, 16U, hour_now,
                                        false, &reason));
    CHECK(reason == IOT_SPOOL_REASON_BATCH_READY);

    /* The query rotates a local copy; the real state changes only on success. */
    CHECK(state.flushes_in_hour == IOT_SPOOL_MAX_FLUSHES_PER_HOUR);
    CHECK(state.window_hour_start_ms == old_time);

    iot_spool_policy_on_flush(&state, hour_now);
    CHECK(state.window_hour_start_ms == hour_now);
    CHECK(state.flushes_in_hour == 1U);
    CHECK(state.window_day_start_ms == old_time);
    CHECK(state.flushes_in_day == 2U);
    CHECK(state.last_flush_ms == hour_now);

    const uint32_t day_now = old_time + 86400000U;
    state = fresh_state();
    state.window_hour_start_ms = old_time;
    state.window_day_start_ms = old_time;
    state.flushes_in_hour = 1U;
    state.flushes_in_day = IOT_SPOOL_MAX_FLUSHES_PER_DAY;

    CHECK(iot_spool_policy_should_flush(&state, day_now, 16U, day_now,
                                        false, &reason));
    iot_spool_policy_on_flush(&state, day_now);
    CHECK(state.window_hour_start_ms == day_now);
    CHECK(state.window_day_start_ms == day_now);
    CHECK(state.flushes_in_hour == 1U);
    CHECK(state.flushes_in_day == 1U);
}

static void test_tick_rollover_thresholds(void)
{
    const uint32_t old_time = 0xffffff00U;
    iot_spool_policy_state_t state = fresh_state();
    iot_spool_reason_t reason = IOT_SPOOL_REASON_NONE;

    state.window_hour_start_ms = old_time;
    state.window_day_start_ms = old_time;
    state.last_flush_ms = old_time;
    state.have_last_flush = true;

    /* Age crosses both the 32-bit tick wrap and the 120 s age threshold. */
    CHECK(iot_spool_policy_should_flush(&state, old_time + 120000U, 1U,
                                        old_time, false, &reason));
    CHECK(reason == IOT_SPOOL_REASON_MAX_AGE);

    /* A batch is ready, but still inside the 60 s minimum interval. */
    CHECK(!iot_spool_policy_should_flush(&state, old_time + 59999U, 16U,
                                         old_time, false, &reason));
    CHECK(reason == IOT_SPOOL_REASON_MIN_INTERVAL_WAIT);

    /* At exactly 60 seconds, the batch can proceed. */
    CHECK(iot_spool_policy_should_flush(&state, old_time + 60000U, 16U,
                                        old_time, false, &reason));
    CHECK(reason == IOT_SPOOL_REASON_BATCH_READY);
}

static void test_counter_saturation(void)
{
    iot_spool_policy_state_t state = fresh_state();
    iot_spool_reason_t reason = IOT_SPOOL_REASON_NONE;

    state.window_hour_start_ms = 100U;
    state.window_day_start_ms = 100U;
    state.flushes_in_hour = UINT16_MAX;
    state.flushes_in_day = UINT16_MAX;

    iot_spool_policy_on_flush(&state, 100U);
    CHECK(state.flushes_in_hour == UINT16_MAX);
    CHECK(state.flushes_in_day == UINT16_MAX);
    CHECK(!iot_spool_policy_should_flush(&state, 100U, 16U, 100U, true,
                                         &reason));
    CHECK(reason == IOT_SPOOL_REASON_HOUR_BUDGET);
}

int main(void)
{
    test_invalid_and_empty();
    test_batch_and_age_flush();
    test_min_interval_and_forced();
    test_budgets();
    test_window_rotation_lifecycle();
    test_tick_rollover_thresholds();
    test_counter_saturation();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all spool policy checks passed\n");
    return 0;
}
