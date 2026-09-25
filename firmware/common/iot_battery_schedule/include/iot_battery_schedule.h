/*
 * Battery node wake schedule policy.
 *
 * Pure C and host-testable. It computes the next monotonic-clock action for a
 * battery-powered Zigbee sensor but performs no I/O and does not directly enter
 * sleep; integration with ZBOSS or deep sleep is a later hardware-validated
 * stage.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IOT_BATTERY_ACTION_INVALID = 0,
    IOT_BATTERY_ACTION_COMMISSION,
    IOT_BATTERY_ACTION_POLL,
    IOT_BATTERY_ACTION_SAMPLE,
    IOT_BATTERY_ACTION_POLL_THEN_SAMPLE,
    IOT_BATTERY_ACTION_LOCAL_EVENT,
} iot_battery_action_t;

typedef struct {
    uint32_t now_ms;
    uint32_t last_poll_ms;
    uint32_t last_sample_ms;
    uint32_t poll_interval_ms;
    uint32_t sample_interval_ms;
    uint32_t next_commission_ms;
    bool joined;
    bool have_commission_deadline;
    bool local_event;
} iot_battery_schedule_input_t;

typedef struct {
    uint32_t delay_ms;
    iot_battery_action_t action;
    bool poll_due;
    bool sample_due;
} iot_battery_schedule_action_t;

/*
 * Time contract:
 * - all timestamps are milliseconds from the same monotonic uptime clock;
 * - timestamps use natural uint32_t wrap-around at the tick origin;
 * - poll/sample intervals must be in the range 1..INT32_MAX milliseconds;
 * - a commission deadline is treated as future only within the same half-range
 *   horizon; a deadline at or before now is executed immediately (delay_ms=0).
 *
 * Before joining, COMMISSION is always selected. A local_event is not sent in
 * that state: the integration layer must latch/clear it and may retry after
 * commissioning succeeds.
 *
 * After joining, LOCAL_EVENT is returned immediately when local_event is set.
 * That action only asks the integration layer to handle/debounce the local
 * event; it does not authorize a network publication. poll_due/sample_due are
 * still calculated so a wake can be merged with periodic work.
 *
 * poll_interval_ms must be a parent/coordinator-safe interval; it is not a
 * generic request for a longer parent timeout.
 */
bool iot_battery_schedule_next(
    const iot_battery_schedule_input_t *input,
    iot_battery_schedule_action_t *action);

#ifdef __cplusplus
}
#endif