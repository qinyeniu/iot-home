#include "iot_provisioning_recovery_quiesce.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

#define CHECK(condition) do {                                         \
    if (!(condition)) {                                               \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,    \
               __LINE__);                                             \
        ++s_failures;                                                 \
    }                                                                 \
} while (0)

static void test_initial_and_invalid_args(void)
{
    iot_recovery_quiesce_t state;

    CHECK(iot_recovery_quiesce_init(&state) == IOT_RECOVERY_QUIESCE_OK);
    CHECK(!iot_recovery_quiesce_operation_active(&state));
    CHECK(iot_recovery_quiesce_is_clear(&state));
    CHECK(iot_recovery_quiesce_init(NULL) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);

    CHECK(iot_recovery_quiesce_begin_operation(NULL, 1U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);
    CHECK(iot_recovery_quiesce_begin_operation(&state, 0U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);
    CHECK(iot_recovery_quiesce_request(NULL, 1U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);
    CHECK(iot_recovery_quiesce_request(&state, 0U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);

    bool allow = false;
    CHECK(iot_recovery_quiesce_end_operation(NULL, 1U, &allow) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);
    CHECK(iot_recovery_quiesce_end_operation(&state, 1U, NULL) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);
    CHECK(iot_recovery_quiesce_end_operation(&state, 1U, &allow) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE);
    CHECK(iot_recovery_quiesce_cancel_request(&state, 1U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE);
}

static void test_normal_operation(void)
{
    iot_recovery_quiesce_t state;
    bool allow = false;

    CHECK(iot_recovery_quiesce_init(&state) == IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_begin_operation(&state, 1U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_operation_active(&state));
    CHECK(!iot_recovery_quiesce_is_clear(&state));
    CHECK(iot_recovery_quiesce_begin_operation(&state, 2U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE);

    CHECK(iot_recovery_quiesce_end_operation(&state, 2U, &allow) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE);
    CHECK(iot_recovery_quiesce_end_operation(&state, 1U, &allow) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(allow);
    CHECK(!iot_recovery_quiesce_operation_active(&state));
    CHECK(iot_recovery_quiesce_is_clear(&state));

    /* IDs are one-use even after reset and after the operation completes. */
    CHECK(iot_recovery_quiesce_begin_operation(&state, 1U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);
    CHECK(iot_recovery_quiesce_reset(&state) == IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_begin_operation(&state, 2U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_end_operation(&state, 2U, &allow) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_begin_operation(&state, 2U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_ARG);
}

static void test_request_when_clear_then_blocks_operations(void)
{
    iot_recovery_quiesce_t state;
    bool allow = false;

    CHECK(iot_recovery_quiesce_init(&state) == IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_request(&state, 10U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(!iot_recovery_quiesce_is_clear(&state));
    CHECK(iot_recovery_quiesce_begin_operation(&state, 1U) ==
          IOT_RECOVERY_QUIESCE_BUSY);
    CHECK(!iot_recovery_quiesce_operation_active(&state));
    CHECK(iot_recovery_quiesce_end_operation(&state, 1U, &allow) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE);
    CHECK(iot_recovery_quiesce_reset(&state) == IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_is_clear(&state));
}

static void test_request_during_active_operation(void)
{
    iot_recovery_quiesce_t state;
    bool allow = true;

    CHECK(iot_recovery_quiesce_init(&state) == IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_begin_operation(&state, 1U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_request(&state, 20U) ==
          IOT_RECOVERY_QUIESCE_BUSY);
    CHECK(iot_recovery_quiesce_operation_active(&state));

    /* A new operation cannot start during the existing active operation. */
    CHECK(iot_recovery_quiesce_begin_operation(&state, 2U) ==
          IOT_RECOVERY_QUIESCE_BUSY);

    /* Repeating the same quiesce request remains BUSY until operation ends. */
    CHECK(iot_recovery_quiesce_request(&state, 20U) ==
          IOT_RECOVERY_QUIESCE_BUSY);

    CHECK(iot_recovery_quiesce_end_operation(&state, 1U, &allow) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(!allow);
    CHECK(!iot_recovery_quiesce_operation_active(&state));
    CHECK(!iot_recovery_quiesce_is_clear(&state));

    /* Same request now completes and normal recovery may be acquired over. */
    CHECK(iot_recovery_quiesce_request(&state, 20U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(!iot_recovery_quiesce_is_clear(&state));

    /* Normal recovery stays blocked until the provisioning flow resets it. */
    CHECK(iot_recovery_quiesce_begin_operation(&state, 3U) ==
          IOT_RECOVERY_QUIESCE_BUSY);
    CHECK(iot_recovery_quiesce_reset(&state) == IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_begin_operation(&state, 3U) ==
          IOT_RECOVERY_QUIESCE_OK);
}

static void test_cancel_and_request_id_edges(void)
{
    iot_recovery_quiesce_t state;

    CHECK(iot_recovery_quiesce_init(&state) == IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_request(&state, 30U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_cancel_request(&state, 30U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_is_clear(&state));

    CHECK(iot_recovery_quiesce_begin_operation(&state, 1U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_request(&state, 30U) ==
          IOT_RECOVERY_QUIESCE_BUSY);
    CHECK(iot_recovery_quiesce_cancel_request(&state, 31U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE);
    CHECK(iot_recovery_quiesce_cancel_request(&state, 30U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE);

    bool allow = false;
    CHECK(iot_recovery_quiesce_end_operation(&state, 1U, &allow) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(!allow);
    CHECK(iot_recovery_quiesce_cancel_request(&state, 30U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_is_clear(&state));

    CHECK(iot_recovery_quiesce_begin_operation(&state, 2U) ==
          IOT_RECOVERY_QUIESCE_OK);
    CHECK(iot_recovery_quiesce_request(&state, 30U) ==
          IOT_RECOVERY_QUIESCE_BUSY);
    CHECK(iot_recovery_quiesce_request(&state, 31U) ==
          IOT_RECOVERY_QUIESCE_ERR_INVALID_STATE);
}

int main(void)
{
    test_initial_and_invalid_args();
    test_normal_operation();
    test_request_when_clear_then_blocks_operations();
    test_request_during_active_operation();
    test_cancel_and_request_id_edges();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all recovery quiesce checks passed\n");
    return 0;
}
