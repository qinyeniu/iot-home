#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Result of one active RF environment scan. */
typedef enum {
    IOT_WIFI_RECOVERY_SCAN_OK = 0,
    IOT_WIFI_RECOVERY_SCAN_EMPTY,
    IOT_WIFI_RECOVERY_SCAN_FAILED,
} iot_wifi_recovery_scan_result_t;

/** Boot watchdog action selected from connection and RF state. */
typedef enum {
    IOT_WIFI_BOOT_ACTION_NONE = 0,
    IOT_WIFI_BOOT_ACTION_HOLD_TARGET_MISSING,
    IOT_WIFI_BOOT_ACTION_REBOOT,
    IOT_WIFI_BOOT_ACTION_REBOOT_LIMIT_REACHED,
} iot_wifi_boot_action_t;

/** Inputs for the cold-boot Wi-Fi watchdog decision. */
typedef struct {
    bool ever_connected;
    uint64_t uptime_ms;
    uint64_t grace_ms;
    bool environment_alive;
    int previous_reboots;
    int max_reboots;
} iot_wifi_boot_input_t;

/** Result of the cold-boot Wi-Fi watchdog decision. */
typedef struct {
    iot_wifi_boot_action_t action;
    int next_reboots;
} iot_wifi_boot_decision_t;

/** Return true when a reconnect retry should also run an RF health scan. */
bool iot_wifi_recovery_health_scan_due(uint32_t retry_count);

/** Predict whether an empty scan would reach the configured recovery limit. */
bool iot_wifi_recovery_empty_scan_will_reach_limit(
    int empty_scans,
    int empty_limit);

/**
 * Apply one scan result to the recovery counters.
 *
 * OK resets the empty-scan counter; EMPTY increments it; FAILED leaves the
 * existing evidence unchanged. When EMPTY reaches empty_limit, deep_sleep_due
 * is set to true. The caller remains responsible for logging and performing
 * hardware recovery.
 */
bool iot_wifi_recovery_apply_scan_result(
    iot_wifi_recovery_scan_result_t result,
    int empty_limit,
    int *empty_scans,
    bool *environment_alive,
    bool *deep_sleep_due);

/**
 * Decide whether the cold-boot Wi-Fi watchdog should reboot the gateway.
 *
 * The function is pure: it does not read the RTC counter, restart the board,
 * or access Wi-Fi hardware. The caller persists next_reboots only when the
 * returned action is IOT_WIFI_BOOT_ACTION_REBOOT.
 */
bool iot_wifi_recovery_decide_boot(
    const iot_wifi_boot_input_t *input,
    iot_wifi_boot_decision_t *decision);

#ifdef __cplusplus
}
#endif
