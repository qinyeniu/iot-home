/*
 * Battery node wake schedule policy.
 */

#include "iot_battery_schedule.h"

#include <limits.h>

static uint32_t elapsed_ms(uint32_t now_ms, uint32_t past_ms)
{
    return now_ms - past_ms;
}

static uint32_t remaining_ms(uint32_t now_ms, uint32_t deadline_ms)
{
    uint32_t remaining = deadline_ms - now_ms;

    /* Values above half the 32-bit tick range are in the past. */
    return remaining <= (uint32_t)INT32_MAX ? remaining : 0U;
}

static bool due(uint32_t now_ms, uint32_t last_ms, uint32_t interval_ms)
{
    return elapsed_ms(now_ms, last_ms) >= interval_ms;
}

bool iot_battery_schedule_next(
    const iot_battery_schedule_input_t *input,
    iot_battery_schedule_action_t *action)
{
    bool poll_due = false;
    bool sample_due = false;
    uint32_t poll_deadline;
    uint32_t sample_deadline;
    uint32_t poll_wait;
    uint32_t sample_wait;

    if (input == NULL || action == NULL ||
        input->poll_interval_ms == 0U ||
        input->sample_interval_ms == 0U ||
        input->poll_interval_ms > (uint32_t)INT32_MAX ||
        input->sample_interval_ms > (uint32_t)INT32_MAX) {
        return false;
    }

    action->delay_ms = 0U;
    action->action = IOT_BATTERY_ACTION_INVALID;
    action->poll_due = false;
    action->sample_due = false;

    /*
     * Before joining, a local event can only be latched because network send is
     * impossible; commissioning remains the first action.
     */
    if (!input->joined) {
        uint32_t delay = 0U;

        if (input->have_commission_deadline) {
            delay = remaining_ms(input->now_ms, input->next_commission_ms);
        }
        action->delay_ms = delay;
        action->action = IOT_BATTERY_ACTION_COMMISSION;
        return true;
    }

    poll_due = due(input->now_ms, input->last_poll_ms,
                    input->poll_interval_ms);
    sample_due = due(input->now_ms, input->last_sample_ms,
                     input->sample_interval_ms);
    action->poll_due = poll_due;
    action->sample_due = sample_due;

    /*
     * Local event handling is immediate, but periodic poll/sample flags remain
     * available so the integration layer can combine operations. LOCAL_EVENT does
     * not itself authorize or perform a network publication.
     */
    if (input->local_event) {
        action->delay_ms = 0U;
        action->action = IOT_BATTERY_ACTION_LOCAL_EVENT;
        return true;
    }

    if (poll_due && sample_due) {
        action->delay_ms = 0U;
        action->action = IOT_BATTERY_ACTION_POLL_THEN_SAMPLE;
        return true;
    }
    if (poll_due) {
        action->delay_ms = 0U;
        action->action = IOT_BATTERY_ACTION_POLL;
        return true;
    }
    if (sample_due) {
        action->delay_ms = 0U;
        action->action = IOT_BATTERY_ACTION_SAMPLE;
        return true;
    }

    poll_deadline = input->last_poll_ms + input->poll_interval_ms;
    sample_deadline = input->last_sample_ms + input->sample_interval_ms;
    poll_wait = remaining_ms(input->now_ms, poll_deadline);
    sample_wait = remaining_ms(input->now_ms, sample_deadline);

    if (poll_wait < sample_wait) {
        action->delay_ms = poll_wait;
        action->action = IOT_BATTERY_ACTION_POLL;
    } else if (sample_wait < poll_wait) {
        action->delay_ms = sample_wait;
        action->action = IOT_BATTERY_ACTION_SAMPLE;
    } else {
        action->delay_ms = poll_wait;
        action->action = IOT_BATTERY_ACTION_POLL_THEN_SAMPLE;
    }
    return true;
}
