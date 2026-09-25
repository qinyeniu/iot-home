#include "iot_battery_schedule.h"

#include <limits.h>
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

static iot_battery_schedule_input_t base_input(void)
{
    iot_battery_schedule_input_t input;

    memset(&input, 0, sizeof(input));
    input.now_ms = 100000U;
    input.last_poll_ms = 40000U;
    input.last_sample_ms = 40000U;
    input.poll_interval_ms = 60000U;
    input.sample_interval_ms = 120000U;
    input.joined = true;
    return input;
}

static void test_invalid_arguments(void)
{
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    CHECK(!iot_battery_schedule_next(NULL, &action));
    CHECK(!iot_battery_schedule_next(&input, NULL));

    input.poll_interval_ms = 0U;
    CHECK(!iot_battery_schedule_next(&input, &action));

    input = base_input();
    input.sample_interval_ms = 0U;
    CHECK(!iot_battery_schedule_next(&input, &action));

    input = base_input();
    input.poll_interval_ms = (uint32_t)INT32_MAX + 1U;
    CHECK(!iot_battery_schedule_next(&input, &action));

    input = base_input();
    input.sample_interval_ms = (uint32_t)INT32_MAX + 1U;
    CHECK(!iot_battery_schedule_next(&input, &action));
}

static void test_commission_schedule(void)
{
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    input.joined = false;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_COMMISSION);
    CHECK(action.delay_ms == 0U);
    CHECK(!action.poll_due);
    CHECK(!action.sample_due);

    input.have_commission_deadline = true;
    input.next_commission_ms = input.now_ms + 5000U;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_COMMISSION);
    CHECK(action.delay_ms == 5000U);
}

static void test_commission_deadline_edges(void)
{
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    input.joined = false;
    input.have_commission_deadline = true;

    /* A missed deadline must be handled immediately, not after wrap-around. */
    input.next_commission_ms = input.now_ms - 1000U;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_COMMISSION);
    CHECK(action.delay_ms == 0U);

    input.next_commission_ms = input.now_ms;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_COMMISSION);
    CHECK(action.delay_ms == 0U);

    /* Future deadline crossing the 32-bit tick origin. */
    input.now_ms = 0xfffff000U;
    input.next_commission_ms = 0x00000400U;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_COMMISSION);
    CHECK(action.delay_ms == 0x1400U);

    /* Deadline in the past across the tick origin. */
    input.now_ms = 0x00000400U;
    input.next_commission_ms = 0xfffff000U;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_COMMISSION);
    CHECK(action.delay_ms == 0U);
}

static void test_unjoined_local_event_is_held_for_commission(void)
{
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    input.joined = false;
    input.local_event = true;
    input.have_commission_deadline = true;
    input.next_commission_ms = input.now_ms + 5000U;

    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_COMMISSION);
    CHECK(action.delay_ms == 5000U);
    CHECK(!action.poll_due);
    CHECK(!action.sample_due);
}

static void test_poll_and_sample_due(void)
{
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    /* now=100000, last=40000, poll interval=60000. */
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_POLL);
    CHECK(action.delay_ms == 0U);
    CHECK(action.poll_due);
    CHECK(!action.sample_due);

    input.now_ms = 160000U;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_POLL_THEN_SAMPLE);
    CHECK(action.delay_ms == 0U);
    CHECK(action.poll_due);
    CHECK(action.sample_due);

    input.poll_interval_ms = 180000U;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_SAMPLE);
    CHECK(action.sample_due);
    CHECK(!action.poll_due);
}

static void test_future_schedule(void)
{
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    input.now_ms = 50000U;
    /* poll deadline=100000, sample deadline=160000. */
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_POLL);
    CHECK(action.delay_ms == 50000U);

    input.last_sample_ms = 0xffffb1e0U;
    /* sample deadline also becomes 100000 across the tick origin. */
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_POLL_THEN_SAMPLE);
    CHECK(action.delay_ms == 50000U);
    CHECK(!action.poll_due);
    CHECK(!action.sample_due);
}

static void test_local_event_without_due_work(void)
{
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    input.last_poll_ms = 90000U;
    input.last_sample_ms = 90000U;
    input.local_event = true;

    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_LOCAL_EVENT);
    CHECK(action.delay_ms == 0U);
    CHECK(!action.poll_due);
    CHECK(!action.sample_due);
}

static void test_local_event_keeps_due_flags(void)
{
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    /* Poll is due, sample is not. */
    input.local_event = true;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_LOCAL_EVENT);
    CHECK(action.delay_ms == 0U);
    CHECK(action.poll_due);
    CHECK(!action.sample_due);

    /* Sample is due, poll is not. */
    input = base_input();
    input.now_ms = 160000U;
    input.poll_interval_ms = 180000U;
    input.local_event = true;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_LOCAL_EVENT);
    CHECK(!action.poll_due);
    CHECK(action.sample_due);

    /* Both are due. */
    input = base_input();
    input.now_ms = 160000U;
    input.local_event = true;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_LOCAL_EVENT);
    CHECK(action.poll_due);
    CHECK(action.sample_due);
}

static void test_tick_rollover(void)
{
    const uint32_t old_time = 0xffffff00U;
    iot_battery_schedule_input_t input = base_input();
    iot_battery_schedule_action_t action;

    input.last_poll_ms = old_time;
    input.last_sample_ms = old_time;
    input.now_ms = old_time + 60000U;
    input.poll_interval_ms = 60000U;
    input.sample_interval_ms = 120000U;

    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_POLL);
    CHECK(action.delay_ms == 0U);
    CHECK(action.poll_due);
    CHECK(!action.sample_due);

    input.now_ms = old_time + 120000U;
    CHECK(iot_battery_schedule_next(&input, &action));
    CHECK(action.action == IOT_BATTERY_ACTION_POLL_THEN_SAMPLE);
    CHECK(action.delay_ms == 0U);
    CHECK(action.poll_due);
    CHECK(action.sample_due);
}

int main(void)
{
    test_invalid_arguments();
    test_commission_schedule();
    test_commission_deadline_edges();
    test_unjoined_local_event_is_held_for_commission();
    test_poll_and_sample_due();
    test_future_schedule();
    test_local_event_without_due_work();
    test_local_event_keeps_due_flags();
    test_tick_rollover();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all battery schedule checks passed\n");
    return 0;
}