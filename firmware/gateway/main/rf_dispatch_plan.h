// CQ-2b: host-testable plan for the single RF dispatch consumer.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rf_command_queue.h"

#define RF_DISPATCH_COMMAND_QUERY 1u
#define RF_DISPATCH_COMMAND_SET 2u
#define RF_DISPATCH_MODE_AUTO 0u
#define RF_DISPATCH_MODE_MANUAL 1u
#define RF_DISPATCH_MODE_UNKNOWN 0xffu
#define RF_DISPATCH_POWER_UNKNOWN_DBM (-128)

typedef enum {
    RF_DISPATCH_PLAN_SEND = 0,
    RF_DISPATCH_PLAN_NO_SEND_POLICY_SAVED,
    RF_DISPATCH_PLAN_ERR_INVALID_ARG,
    RF_DISPATCH_PLAN_ERR_EXPIRED,
    RF_DISPATCH_PLAN_ERR_WAITING,
    RF_DISPATCH_PLAN_ERR_INACTIVE,
} rf_dispatch_plan_status_t;

typedef struct {
    uint32_t now_ms;
    uint16_t node_addr;
    int8_t node_power_dbm;
    bool report_waiting;
    uint32_t wait_deadline_ms;
    uint8_t packet_sequence;
} rf_dispatch_plan_state_t;

typedef struct {
    rf_dispatch_plan_status_t status;
    uint16_t node_addr;
    uint8_t seq;
    uint8_t command;
    int8_t power_dbm;
    uint8_t mode;
    bool publish_querying;
    bool clear_waiting;
    int8_t expected_report_power_dbm;
    uint8_t expected_report_mode;
} rf_dispatch_plan_t;

typedef struct {
    uint8_t intent_command;
    uint8_t desired_mode;
    int8_t target_power_dbm;
    bool policy_apply_pending;
    int8_t reported_power_dbm;
    uint8_t reported_mode;
    bool matched_transaction;
    bool node_initiated_report;
} rf_dispatch_after_report_state_t;

typedef enum {
    RF_DISPATCH_AFTER_REPORT_NONE = 0,
    RF_DISPATCH_AFTER_REPORT_COMPLETE,
    RF_DISPATCH_AFTER_REPORT_RETRY,
} rf_dispatch_after_report_result_t;

/*
 * Creates the consumer's pre-send plan without calling FreeRTOS, ZBOSS, NVS or
 * network APIs. The caller must hold the RF state lock while reading state and
 * must release that lock before acquiring the ZBOSS lock or sending.
 *
 * The policy RAM update and persistence is performed by the producer before
 * queueing. This function only decides whether a packet is needed, reserves the
 * next low packet sequence, and describes the waiting report to install after
 * esp_zb_aps_data_request() succeeds.
 *
 * A MANUAL/AUTO command supersedes an old waiting transaction. A QUERY is
 * rejected while a non-expired transaction is waiting.
 */
bool rf_dispatch_plan_create(const rf_dispatch_cmd_t *cmd,
                             const rf_dispatch_plan_state_t *state,
                             rf_dispatch_plan_t *plan);

/*
 * Decides whether an accepted report completes the user intent or requires
 * another RF attempt. A gateway-bound old private reply is authoritative only
 * when matched_transaction is true; a fresh node-initiated report may complete a
 * SET if its value already satisfies the desired policy.
 */
bool rf_dispatch_after_report_create(
    const rf_dispatch_after_report_state_t *state,
    rf_dispatch_after_report_result_t *result);
