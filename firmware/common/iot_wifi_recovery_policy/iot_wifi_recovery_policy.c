#include "iot_wifi_recovery_policy.h"

#include <limits.h>

bool iot_wifi_recovery_health_scan_due(uint32_t retry_count)
{
    return (retry_count >= 2U && retry_count <= 4U) ||
           (retry_count >= 8U && (retry_count % 4U) == 0U);
}

bool iot_wifi_recovery_empty_scan_will_reach_limit(
    int empty_scans,
    int empty_limit)
{
    return empty_scans >= 0 && empty_limit > 0 &&
           empty_scans >= empty_limit - 1;
}

bool iot_wifi_recovery_apply_scan_result(
    iot_wifi_recovery_scan_result_t result,
    int empty_limit,
    int *empty_scans,
    bool *environment_alive,
    bool *deep_sleep_due)
{
    if (empty_scans == NULL || environment_alive == NULL ||
        deep_sleep_due == NULL || empty_limit <= 0 ||
        *empty_scans < 0) {
        return false;
    }

    *deep_sleep_due = false;

    switch (result) {
    case IOT_WIFI_RECOVERY_SCAN_OK:
        *empty_scans = 0;
        *environment_alive = true;
        return true;

    case IOT_WIFI_RECOVERY_SCAN_FAILED:
        return true;

    case IOT_WIFI_RECOVERY_SCAN_EMPTY:
        *environment_alive = false;
        if (*empty_scans < INT_MAX) {
            ++*empty_scans;
        }
        *deep_sleep_due = (*empty_scans >= empty_limit);
        return true;

    default:
        return false;
    }
}

bool iot_wifi_recovery_decide_boot(
    const iot_wifi_boot_input_t *input,
    iot_wifi_boot_decision_t *decision)
{
    if (input == NULL || decision == NULL ||
        input->previous_reboots < 0 || input->max_reboots < 0) {
        return false;
    }

    decision->next_reboots = input->previous_reboots;
    decision->action = IOT_WIFI_BOOT_ACTION_NONE;

    if (input->ever_connected ||
        input->uptime_ms <= input->grace_ms) {
        return true;
    }

    if (input->environment_alive) {
        decision->action = IOT_WIFI_BOOT_ACTION_HOLD_TARGET_MISSING;
        return true;
    }

    if (input->previous_reboots < input->max_reboots) {
        decision->action = IOT_WIFI_BOOT_ACTION_REBOOT;
        decision->next_reboots = input->previous_reboots + 1;
        return true;
    }

    decision->action = IOT_WIFI_BOOT_ACTION_REBOOT_LIMIT_REACHED;
    return true;
}
