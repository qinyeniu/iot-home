#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Why a periodic evaluation asks the gateway to send QUERY. */
typedef enum {
    IOT_RF_TRANSACTION_QUERY_NONE = 0,
    IOT_RF_TRANSACTION_QUERY_CURRENT_UNKNOWN = 1,
    IOT_RF_TRANSACTION_QUERY_WAIT_TIMEOUT = 2,
    IOT_RF_TRANSACTION_QUERY_RECONCILE = 3,
} iot_rf_transaction_query_reason_t;

/** Periodic transaction decision. */
typedef enum {
    IOT_RF_TRANSACTION_DECISION_STALE = 0,
    IOT_RF_TRANSACTION_DECISION_QUERY = 1,
    IOT_RF_TRANSACTION_DECISION_WAIT = 2,
    IOT_RF_TRANSACTION_DECISION_CONTINUE = 3,
} iot_rf_transaction_decision_type_t;

/** Timing limits, all in the caller's millisecond time base. */
typedef struct {
    uint32_t link_fresh_ms;
    uint32_t report_timeout_ms;
    uint32_t reconcile_ms;
} iot_rf_transaction_config_t;

/** Snapshot consumed by one periodic transaction evaluation. */
typedef struct {
    uint32_t now_ms;
    uint32_t last_rx_ms;
    uint32_t wait_started_ms;
    uint32_t last_confirmed_ms;
    bool report_waiting;
    bool current_power_known;
} iot_rf_transaction_input_t;

/** Result of one periodic transaction evaluation. */
typedef struct {
    iot_rf_transaction_decision_type_t type;
    iot_rf_transaction_query_reason_t query_reason;
    uint32_t link_age_ms;
} iot_rf_transaction_decision_t;

/** Expected REPORT fields used to match a node's response. */
typedef struct {
    uint8_t sequence;
    int8_t power_dbm;
    uint8_t mode;
} iot_rf_transaction_expected_t;

/** REPORT fields received from a node. */
typedef struct {
    uint8_t sequence;
    int8_t power_dbm;
    uint8_t mode;
} iot_rf_transaction_report_t;

/** Validate that all transaction timing values are non-zero. */
bool iot_rf_transaction_config_valid(
    const iot_rf_transaction_config_t *config);

/**
 * Evaluate the transaction timing state.
 *
 * The function is pure and uses unsigned 32-bit subtraction, so normal tick
 * rollover is handled consistently by the caller's time base.
 */
bool iot_rf_transaction_evaluate(
    const iot_rf_transaction_config_t *config,
    const iot_rf_transaction_input_t *input,
    iot_rf_transaction_decision_t *decision);

/**
 * Return true for an exact V2 response.
 *
 * A power/mode value of -128/0xFF means "do not require this field"; sequence
 * must always match. The caller is responsible for confirming it is waiting
 * and that the packet version is V2.
 */
bool iot_rf_transaction_report_matches(
    const iot_rf_transaction_expected_t *expected,
    const iot_rf_transaction_report_t *report);

#ifdef __cplusplus
}
#endif
