#include "iot_wifi_recovery_policy.h"

#include <limits.h>
#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void boot_input_init(iot_wifi_boot_input_t *input)
{
    input->ever_connected = false;
    input->uptime_ms = 30000U;
    input->grace_ms = 25000U;
    input->environment_alive = false;
    input->previous_reboots = 0;
    input->max_reboots = 2;
}

int main(void)
{
    CHECK(!iot_wifi_recovery_health_scan_due(0));
    CHECK(!iot_wifi_recovery_health_scan_due(1));
    CHECK(iot_wifi_recovery_health_scan_due(2));
    CHECK(iot_wifi_recovery_health_scan_due(3));
    CHECK(iot_wifi_recovery_health_scan_due(4));
    CHECK(!iot_wifi_recovery_health_scan_due(5));
    CHECK(!iot_wifi_recovery_health_scan_due(7));
    CHECK(iot_wifi_recovery_health_scan_due(8));
    CHECK(!iot_wifi_recovery_health_scan_due(9));
    CHECK(iot_wifi_recovery_health_scan_due(12));
    CHECK(iot_wifi_recovery_health_scan_due(16));

    CHECK(!iot_wifi_recovery_empty_scan_will_reach_limit(-1, 3));
    CHECK(!iot_wifi_recovery_empty_scan_will_reach_limit(0, 0));
    CHECK(!iot_wifi_recovery_empty_scan_will_reach_limit(0, 3));
    CHECK(iot_wifi_recovery_empty_scan_will_reach_limit(2, 3));
    CHECK(iot_wifi_recovery_empty_scan_will_reach_limit(3, 3));
    CHECK(iot_wifi_recovery_empty_scan_will_reach_limit(INT_MAX, 3));
    CHECK(iot_wifi_recovery_empty_scan_will_reach_limit(INT_MAX - 1, INT_MAX));

    int empty_scans = 0;
    bool environment_alive = false;
    bool deep_sleep_due = true;
    CHECK(iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_OK, 3, &empty_scans,
        &environment_alive, &deep_sleep_due));
    CHECK(empty_scans == 0);
    CHECK(environment_alive);
    CHECK(!deep_sleep_due);

    environment_alive = true;
    CHECK(iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_FAILED, 3, &empty_scans,
        &environment_alive, &deep_sleep_due));
    CHECK(empty_scans == 0);
    CHECK(environment_alive);
    CHECK(!deep_sleep_due);

    environment_alive = false;
    CHECK(iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_FAILED, 3, &empty_scans,
        &environment_alive, &deep_sleep_due));
    CHECK(empty_scans == 0);
    CHECK(!environment_alive);
    CHECK(!deep_sleep_due);

    for (int expected = 1; expected <= 2; ++expected) {
        CHECK(iot_wifi_recovery_apply_scan_result(
            IOT_WIFI_RECOVERY_SCAN_EMPTY, 3, &empty_scans,
            &environment_alive, &deep_sleep_due));
        CHECK(empty_scans == expected);
        CHECK(!environment_alive);
        CHECK(!deep_sleep_due);
    }
    CHECK(iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_EMPTY, 3, &empty_scans,
        &environment_alive, &deep_sleep_due));
    CHECK(empty_scans == 3);
    CHECK(!environment_alive);
    CHECK(deep_sleep_due);

    empty_scans = 1;
    environment_alive = true;
    deep_sleep_due = true;
    CHECK(!iot_wifi_recovery_apply_scan_result(
        (iot_wifi_recovery_scan_result_t)99, 3, &empty_scans,
        &environment_alive, &deep_sleep_due));
    CHECK(empty_scans == 1);
    CHECK(environment_alive);
    CHECK(!deep_sleep_due);
    CHECK(!iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_EMPTY, 0, &empty_scans,
        &environment_alive, &deep_sleep_due));
    CHECK(!iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_EMPTY, 3, NULL,
        &environment_alive, &deep_sleep_due));
    CHECK(!iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_EMPTY, 3, &empty_scans,
        NULL, &deep_sleep_due));
    CHECK(!iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_EMPTY, 3, &empty_scans,
        &environment_alive, NULL));
    empty_scans = -1;
    CHECK(!iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_EMPTY, 3, &empty_scans,
        &environment_alive, &deep_sleep_due));

    empty_scans = INT_MAX - 1;
    environment_alive = false;
    CHECK(iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_EMPTY, INT_MAX, &empty_scans,
        &environment_alive, &deep_sleep_due));
    CHECK(empty_scans == INT_MAX);
    CHECK(deep_sleep_due);
    CHECK(iot_wifi_recovery_apply_scan_result(
        IOT_WIFI_RECOVERY_SCAN_EMPTY, INT_MAX, &empty_scans,
        &environment_alive, &deep_sleep_due));
    CHECK(empty_scans == INT_MAX);
    CHECK(deep_sleep_due);

    iot_wifi_boot_input_t input;
    iot_wifi_boot_decision_t decision;

    boot_input_init(&input);
    CHECK(iot_wifi_recovery_decide_boot(&input, &decision));
    CHECK(decision.action == IOT_WIFI_BOOT_ACTION_REBOOT);
    CHECK(decision.next_reboots == 1);

    input.uptime_ms = 25000U;
    CHECK(iot_wifi_recovery_decide_boot(&input, &decision));
    CHECK(decision.action == IOT_WIFI_BOOT_ACTION_NONE);
    CHECK(decision.next_reboots == 0);

    input.uptime_ms = 30000U;
    input.ever_connected = true;
    CHECK(iot_wifi_recovery_decide_boot(&input, &decision));
    CHECK(decision.action == IOT_WIFI_BOOT_ACTION_NONE);

    boot_input_init(&input);
    input.environment_alive = true;
    CHECK(iot_wifi_recovery_decide_boot(&input, &decision));
    CHECK(decision.action == IOT_WIFI_BOOT_ACTION_HOLD_TARGET_MISSING);
    CHECK(decision.next_reboots == 0);

    boot_input_init(&input);
    input.previous_reboots = 2;
    CHECK(iot_wifi_recovery_decide_boot(&input, &decision));
    CHECK(decision.action == IOT_WIFI_BOOT_ACTION_REBOOT_LIMIT_REACHED);
    CHECK(decision.next_reboots == 2);

    boot_input_init(&input);
    input.max_reboots = 0;
    CHECK(iot_wifi_recovery_decide_boot(&input, &decision));
    CHECK(decision.action == IOT_WIFI_BOOT_ACTION_REBOOT_LIMIT_REACHED);

    boot_input_init(&input);
    input.uptime_ms = UINT32_MAX;
    input.grace_ms = UINT32_MAX;
    CHECK(iot_wifi_recovery_decide_boot(&input, &decision));
    CHECK(decision.action == IOT_WIFI_BOOT_ACTION_NONE);
    input.uptime_ms = UINT64_MAX;
    input.grace_ms = UINT64_MAX - 1ULL;
    CHECK(iot_wifi_recovery_decide_boot(&input, &decision));
    CHECK(decision.action == IOT_WIFI_BOOT_ACTION_REBOOT);
    CHECK(decision.next_reboots == 1);

    CHECK(!iot_wifi_recovery_decide_boot(NULL, &decision));
    CHECK(!iot_wifi_recovery_decide_boot(&input, NULL));
    input.previous_reboots = -1;
    CHECK(!iot_wifi_recovery_decide_boot(&input, &decision));
    boot_input_init(&input);
    input.max_reboots = -1;
    CHECK(!iot_wifi_recovery_decide_boot(&input, &decision));

    printf("wifi recovery policy host test passed\n");
    return 0;
}
