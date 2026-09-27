#include "iot_rf_transaction.h"

#include <stddef.h>

#define RF_TRANSACTION_IGNORE_POWER ((int8_t)-128)
#define RF_TRANSACTION_IGNORE_MODE  ((uint8_t)0xFF)

bool iot_rf_transaction_config_valid(
    const iot_rf_transaction_config_t *config)
{
    return config != NULL &&
           config->link_fresh_ms != 0U &&
           config->report_timeout_ms != 0U &&
           config->reconcile_ms != 0U;
}

bool iot_rf_transaction_evaluate(
    const iot_rf_transaction_config_t *config,
    const iot_rf_transaction_input_t *input,
    iot_rf_transaction_decision_t *decision)
{
    uint32_t link_age;
    bool wait_timed_out;

    if (!iot_rf_transaction_config_valid(config) || input == NULL ||
        decision == NULL) {
        return false;
    }

    link_age = input->now_ms - input->last_rx_ms;
    wait_timed_out = input->report_waiting &&
                     input->wait_started_ms != 0U &&
                     input->now_ms - input->wait_started_ms >=
                         config->report_timeout_ms;

    decision->link_age_ms = link_age;
    decision->query_reason = IOT_RF_TRANSACTION_QUERY_NONE;

    if (link_age > config->link_fresh_ms) {
        decision->type = IOT_RF_TRANSACTION_DECISION_STALE;
        return true;
    }

    if (!input->current_power_known) {
        decision->type = IOT_RF_TRANSACTION_DECISION_QUERY;
        decision->query_reason =
            IOT_RF_TRANSACTION_QUERY_CURRENT_UNKNOWN;
        return true;
    }

    if (wait_timed_out) {
        decision->type = IOT_RF_TRANSACTION_DECISION_QUERY;
        decision->query_reason =
            IOT_RF_TRANSACTION_QUERY_WAIT_TIMEOUT;
        return true;
    }

    if (!input->report_waiting && input->last_confirmed_ms != 0U &&
        input->now_ms - input->last_confirmed_ms >=
            config->reconcile_ms) {
        decision->type = IOT_RF_TRANSACTION_DECISION_QUERY;
        decision->query_reason =
            IOT_RF_TRANSACTION_QUERY_RECONCILE;
        return true;
    }

    if (input->report_waiting) {
        decision->type = IOT_RF_TRANSACTION_DECISION_WAIT;
        return true;
    }

    decision->type = IOT_RF_TRANSACTION_DECISION_CONTINUE;
    return true;
}

bool iot_rf_transaction_report_matches(
    const iot_rf_transaction_expected_t *expected,
    const iot_rf_transaction_report_t *report)
{
    if (expected == NULL || report == NULL) {
        return false;
    }

    return report->sequence == expected->sequence &&
           (expected->power_dbm == RF_TRANSACTION_IGNORE_POWER ||
            report->power_dbm == expected->power_dbm) &&
           (expected->mode == RF_TRANSACTION_IGNORE_MODE ||
            report->mode == expected->mode);
}
