#include "iot_provisioning_wifi_owner.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(condition) do {                                         \
    if (!(condition)) {                                               \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,    \
               __LINE__);                                             \
        ++s_failures;                                                 \
    }                                                                 \
} while (0)

static void test_initial_and_normal_owner(void)
{
    iot_wifi_owner_state_t state;

    CHECK(iot_wifi_owner_init(&state) == IOT_WIFI_OWNER_OK);
    CHECK(!iot_wifi_owner_is_provision(&state));
    CHECK(iot_wifi_owner_normal_recovery_allowed(&state));
    CHECK(iot_wifi_owner_sta_start_connect_allowed(&state));
    CHECK(!iot_wifi_owner_event_is_current(&state, "tmp-0001", 1U));

    CHECK(iot_wifi_owner_init(NULL) == IOT_WIFI_OWNER_ERR_INVALID_ARG);
}

static void test_acquire_gates_actions(void)
{
    iot_wifi_owner_state_t state;

    CHECK(iot_wifi_owner_init(&state) == IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_acquire(&state, "tmp-0001", 1U) ==
          IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_is_provision(&state));
    CHECK(!iot_wifi_owner_normal_recovery_allowed(&state));
    CHECK(!iot_wifi_owner_sta_start_connect_allowed(&state));
    CHECK(iot_wifi_owner_event_is_current(&state, "tmp-0001", 1U));
    CHECK(!iot_wifi_owner_event_is_current(&state, "tmp-0002", 1U));
    CHECK(!iot_wifi_owner_event_is_current(&state, "tmp-0001", 2U));

    CHECK(iot_wifi_owner_acquire(&state, "tmp-0002", 2U) ==
          IOT_WIFI_OWNER_ERR_INVALID_STATE);
    CHECK(iot_wifi_owner_release(&state, 2U) ==
          IOT_WIFI_OWNER_ERR_INVALID_STATE);
}

static void test_release_restores_normal(void)
{
    iot_wifi_owner_state_t state;

    CHECK(iot_wifi_owner_init(&state) == IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_acquire(&state, "tmp-0001", 1U) ==
          IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_release(&state, 1U) == IOT_WIFI_OWNER_OK);
    CHECK(!iot_wifi_owner_is_provision(&state));
    CHECK(iot_wifi_owner_normal_recovery_allowed(&state));
    CHECK(iot_wifi_owner_sta_start_connect_allowed(&state));

    /* Old provisioning events must not affect the restored normal owner. */
    CHECK(!iot_wifi_owner_event_is_current(&state, "tmp-0001", 1U));

    /* The next owner generation must not be reused. */
    CHECK(iot_wifi_owner_acquire(&state, "tmp-0001", 1U) ==
          IOT_WIFI_OWNER_ERR_GENERATION_REUSED);
    CHECK(iot_wifi_owner_acquire(&state, "tmp-0002", 2U) ==
          IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_event_is_current(&state, "tmp-0002", 2U));
    CHECK(iot_wifi_owner_release(&state, 2U) == IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_acquire(&state, "tmp-old", 1U) ==
          IOT_WIFI_OWNER_ERR_GENERATION_REUSED);
}

static void test_invalid_args(void)
{
    iot_wifi_owner_state_t state;
    const char *id32 = "tmp-1234567890123456789012345678";
    const char *id33 = "tmp-12345678901234567890123456789";

    CHECK(iot_wifi_owner_init(&state) == IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_acquire(&state, NULL, 1U) ==
          IOT_WIFI_OWNER_ERR_INVALID_ARG);
    CHECK(iot_wifi_owner_acquire(&state, "", 1U) ==
          IOT_WIFI_OWNER_ERR_INVALID_ARG);
    CHECK(iot_wifi_owner_acquire(&state, id32, 1U) == IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_release(&state, 1U) == IOT_WIFI_OWNER_OK);
    CHECK(iot_wifi_owner_acquire(&state, id33, 2U) ==
          IOT_WIFI_OWNER_ERR_INVALID_ARG);
    CHECK(iot_wifi_owner_acquire(&state, "tmp-0002", 0U) ==
          IOT_WIFI_OWNER_ERR_INVALID_ARG);
}

int main(void)
{
    test_initial_and_normal_owner();
    test_acquire_gates_actions();
    test_release_restores_normal();
    test_invalid_args();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all provisioning wifi owner checks passed\n");
    return 0;
}
