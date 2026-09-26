#include "iot_provisioning_wifi_phase.h"

#include <stdio.h>

static int s_failures;

#define CHECK(condition) do {                                         \
    if (!(condition)) {                                               \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,    \
               __LINE__);                                             \
        ++s_failures;                                                 \
    }                                                                 \
} while (0)

static void test_complete_flow(void)
{
    iot_prov_wifi_phase_state_t state;

    CHECK(iot_prov_wifi_phase_init(&state) == IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_get(&state) == IOT_PROV_WIFI_PHASE_STARTING);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_CONNECT_REQUESTED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_get(&state) == IOT_PROV_WIFI_PHASE_SCANNING);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATION_PROGRESS) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_get(&state) ==
          IOT_PROV_WIFI_PHASE_ASSOCIATING);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_get(&state) ==
          IOT_PROV_WIFI_PHASE_WAITING_IP);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_GOT_IP) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_get(&state) == IOT_PROV_WIFI_PHASE_FINISHED);
}

static void test_association_without_progress(void)
{
    iot_prov_wifi_phase_state_t state;

    CHECK(iot_prov_wifi_phase_init(&state) == IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_CONNECT_REQUESTED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_get(&state) ==
          IOT_PROV_WIFI_PHASE_WAITING_IP);
}

static void test_disconnect_and_reset(void)
{
    iot_prov_wifi_phase_state_t state;

    CHECK(iot_prov_wifi_phase_init(&state) == IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_CONNECT_REQUESTED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_DISCONNECTED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_get(&state) == IOT_PROV_WIFI_PHASE_STARTING);

    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_CONNECT_REQUESTED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATION_PROGRESS) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_RESET) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_get(&state) == IOT_PROV_WIFI_PHASE_STARTING);
}

static void test_timeouts_and_invalid_events(void)
{
    iot_prov_wifi_phase_state_t state;

    CHECK(iot_prov_wifi_phase_init(&state) == IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_timeout_error(&state) ==
          IOT_PROV_ERR_INVALID_STATE);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_CONNECT_REQUESTED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_timeout_error(&state) ==
          IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATION_PROGRESS) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_timeout_error(&state) ==
          IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_ASSOCIATED) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_timeout_error(&state) ==
          IOT_PROV_ERR_WIFI_NO_IP_ADDRESS);
    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_GOT_IP) ==
          IOT_PROV_WIFI_PHASE_OK);
    CHECK(iot_prov_wifi_phase_timeout_error(&state) ==
          IOT_PROV_ERR_INVALID_STATE);

    CHECK(iot_prov_wifi_phase_apply(
              &state, IOT_PROV_WIFI_PHASE_EVENT_GOT_IP) ==
          IOT_PROV_WIFI_PHASE_ERR_INVALID_STATE);
    CHECK(iot_prov_wifi_phase_apply(
              &state, (iot_prov_wifi_phase_event_t)99) ==
          IOT_PROV_WIFI_PHASE_ERR_INVALID_ARG);
    CHECK(iot_prov_wifi_phase_init(NULL) ==
          IOT_PROV_WIFI_PHASE_ERR_INVALID_ARG);
}

int main(void)
{
    test_complete_flow();
    test_association_without_progress();
    test_disconnect_and_reset();
    test_timeouts_and_invalid_events();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all provisioning wifi phase checks passed\n");
    return 0;
}

