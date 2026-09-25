#include "rf_background_schedule.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

#define CHECK(condition) do {     if (!(condition)) {         printf("CHECK failed: %s (%s:%d)", #condition, __FILE__, __LINE__);         ++s_failures;     } } while (0)

static rf_background_admission_state_t base_state(void)
{
    rf_background_admission_state_t state = {
        .now_ms = RF_BACKGROUND_EVAL_MS,
        .last_eval_ms = 0U,
        .node_addr = 0x82cbU,
        .tx_backoff_active = false,
        .user_intent_active = false,
        .report_waiting = false,
        .report_submitting = false,
        .dispatch_active = false,
        .background_command_active = false,
    };
    return state;
}

static void test_invalid_arguments(void)
{
    rf_background_admission_state_t state = base_state();
    rf_background_admission_result_t result;

    CHECK(!rf_background_admission_decide(NULL, &result));
    CHECK(!rf_background_admission_decide(&state, NULL));
    state.now_ms = 0U;
    CHECK(!rf_background_admission_decide(&state, &result));
}

static void test_admit_and_cooldown(void)
{
    rf_background_admission_state_t state = base_state();
    rf_background_admission_result_t result;

    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_ADMISSION_ADMIT);

    state.now_ms = RF_BACKGROUND_EVAL_MS - 1U;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_EVAL_COOLDOWN);

    state.now_ms = RF_BACKGROUND_EVAL_MS;
    state.last_eval_ms = 1U;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_EVAL_COOLDOWN);

    state.now_ms = RF_BACKGROUND_EVAL_MS + 1U;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_ADMISSION_ADMIT);

    // Unsigned modular elapsed time must not mistake a very old timestamp for
    // a future cooldown deadline.
    state.now_ms = UINT32_MAX;
    state.last_eval_ms = 0U;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_ADMISSION_ADMIT);
}

static void test_cooldown_wrap(void)
{
    rf_background_admission_state_t state = base_state();
    rf_background_admission_result_t result;

    state.now_ms = 100U;
    state.last_eval_ms = UINT32_MAX - RF_BACKGROUND_EVAL_MS + 101U;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_ADMISSION_ADMIT);

    state.now_ms = 99U;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_EVAL_COOLDOWN);
}

static void test_reject_priority(void)
{
    rf_background_admission_state_t state = base_state();
    rf_background_admission_result_t result;

    state.tx_backoff_active = true;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_TX_BACKOFF);

    state = base_state();
    state.node_addr = 0U;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_NODE_INACTIVE);

    state = base_state();
    state.user_intent_active = true;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_USER_PRIORITY);

    state = base_state();
    state.report_waiting = true;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_REPORT_WAITING);

    state = base_state();
    state.report_submitting = true;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_REPORT_WAITING);

    state = base_state();
    state.dispatch_active = true;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_DISPATCH_ACTIVE);

    state = base_state();
    state.background_command_active = true;
    CHECK(rf_background_admission_decide(&state, &result));
    CHECK(result == RF_BACKGROUND_REJECT_BACKGROUND_ACTIVE);
}

int main(void)
{
    test_invalid_arguments();
    test_admit_and_cooldown();
    test_cooldown_wrap();
    test_reject_priority();

    if (s_failures != 0) {
        printf("rf_background_schedule host tests: FAIL (%d)", s_failures);
        return 1;
    }
    puts("rf_background_schedule host tests: PASS");
    return 0;
}
