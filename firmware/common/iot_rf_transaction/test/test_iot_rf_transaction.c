#include "iot_rf_transaction.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static iot_rf_transaction_config_t make_config(void)
{
    iot_rf_transaction_config_t config = {
        .link_fresh_ms = 120000U,
        .report_timeout_ms = 90000U,
        .reconcile_ms = 300000U,
    };
    return config;
}

static iot_rf_transaction_input_t make_input(void)
{
    iot_rf_transaction_input_t input = {
        .now_ms = 300000U,
        .last_rx_ms = 200000U,
        .wait_started_ms = 250000U,
        .last_confirmed_ms = 0U,
        .report_waiting = false,
        .current_power_known = true,
    };
    return input;
}

static void evaluate_or_fail(const iot_rf_transaction_config_t *config,
                             const iot_rf_transaction_input_t *input,
                             iot_rf_transaction_decision_t *decision)
{
    assert(iot_rf_transaction_evaluate(config, input, decision));
}

static void test_config_and_invalid_arguments(void)
{
    iot_rf_transaction_config_t config = make_config();
    iot_rf_transaction_input_t input = make_input();
    iot_rf_transaction_decision_t decision;
    iot_rf_transaction_config_t invalid;

    assert(iot_rf_transaction_config_valid(&config));
    evaluate_or_fail(&config, &input, &decision);

    assert(!iot_rf_transaction_evaluate(NULL, &input, &decision));
    assert(!iot_rf_transaction_evaluate(&config, NULL, &decision));
    assert(!iot_rf_transaction_evaluate(&config, &input, NULL));

    invalid = config;
    invalid.link_fresh_ms = 0U;
    assert(!iot_rf_transaction_config_valid(&invalid));

    invalid = config;
    invalid.report_timeout_ms = 0U;
    assert(!iot_rf_transaction_config_valid(&invalid));

    invalid = config;
    invalid.reconcile_ms = 0U;
    assert(!iot_rf_transaction_config_valid(&invalid));
}

static void test_link_freshness_boundary(void)
{
    iot_rf_transaction_config_t config = make_config();
    iot_rf_transaction_input_t input = make_input();
    iot_rf_transaction_decision_t decision;

    input.now_ms = input.last_rx_ms + 120000U;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_CONTINUE);
    assert(decision.link_age_ms == 120000U);

    input.now_ms = input.last_rx_ms + 120001U;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_STALE);
    assert(decision.link_age_ms == 120001U);
}

static void test_unknown_current_queries(void)
{
    iot_rf_transaction_config_t config = make_config();
    iot_rf_transaction_input_t input = make_input();
    iot_rf_transaction_decision_t decision;

    input.current_power_known = false;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_QUERY);
    assert(decision.query_reason ==
           IOT_RF_TRANSACTION_QUERY_CURRENT_UNKNOWN);
}

static void test_wait_timeout_boundary(void)
{
    iot_rf_transaction_config_t config = make_config();
    iot_rf_transaction_input_t input = make_input();
    iot_rf_transaction_decision_t decision;

    input.report_waiting = true;
    input.now_ms = input.wait_started_ms + 89999U;
    input.last_rx_ms = input.now_ms;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_WAIT);

    input.now_ms = input.wait_started_ms + 90000U;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_QUERY);
    assert(decision.query_reason ==
           IOT_RF_TRANSACTION_QUERY_WAIT_TIMEOUT);
}

static void test_reconcile_boundary(void)
{
    iot_rf_transaction_config_t config = make_config();
    iot_rf_transaction_input_t input = make_input();
    iot_rf_transaction_decision_t decision;

    input.last_confirmed_ms = 10U;
    input.now_ms = input.last_confirmed_ms + 299999U;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_CONTINUE);

    input.now_ms = input.last_confirmed_ms + 300000U;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_QUERY);
    assert(decision.query_reason ==
           IOT_RF_TRANSACTION_QUERY_RECONCILE);

    /* Zero confirmation time means there has not yet been a confirmation. */
    input.last_confirmed_ms = 0U;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_CONTINUE);

    /* An active wait suppresses ordinary reconcile. */
    input.last_confirmed_ms = 10U;
    input.report_waiting = true;
    input.wait_started_ms = input.now_ms;
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_WAIT);
}

static void test_tick_rollover(void)
{
    iot_rf_transaction_config_t config = make_config();
    iot_rf_transaction_input_t input = make_input();
    iot_rf_transaction_decision_t decision;

    input.now_ms = 10U;
    input.last_rx_ms = UINT32_MAX - 21U; /* age = 32 ms */
    evaluate_or_fail(&config, &input, &decision);
    assert(decision.type == IOT_RF_TRANSACTION_DECISION_CONTINUE);
    assert(decision.link_age_ms == 32U);
}

static void test_report_matching(void)
{
    const iot_rf_transaction_expected_t expected = {
        .sequence = 7U,
        .power_dbm = -10,
        .mode = 0 /* auto */,
    };
    iot_rf_transaction_report_t report = {
        .sequence = 7U,
        .power_dbm = -10,
        .mode = 0 /* auto */,
    };

    assert(iot_rf_transaction_report_matches(&expected, &report));

    report.sequence = 8U;
    assert(!iot_rf_transaction_report_matches(&expected, &report));
    report.sequence = 7U;

    report.power_dbm = 0;
    assert(!iot_rf_transaction_report_matches(&expected, &report));
    report.power_dbm = -10;

    report.mode = 1 /* manual */;
    assert(!iot_rf_transaction_report_matches(&expected, &report));
    report.mode = 0;

    assert(!iot_rf_transaction_report_matches(NULL, &report));
    assert(!iot_rf_transaction_report_matches(&expected, NULL));
}

static void test_optional_report_fields(void)
{
    const iot_rf_transaction_expected_t expected = {
        .sequence = 9U,
        .power_dbm = -128,
        .mode = 0xFF,
    };
    const iot_rf_transaction_report_t report = {
        .sequence = 9U,
        .power_dbm = 20,
        .mode = 1,
    };

    assert(iot_rf_transaction_report_matches(&expected, &report));
}

int main(void)
{
    test_config_and_invalid_arguments();
    test_link_freshness_boundary();
    test_unknown_current_queries();
    test_wait_timeout_boundary();
    test_reconcile_boundary();
    test_tick_rollover();
    test_report_matching();
    test_optional_report_fields();
    puts("iot_rf_transaction host tests passed");
    return 0;
}
