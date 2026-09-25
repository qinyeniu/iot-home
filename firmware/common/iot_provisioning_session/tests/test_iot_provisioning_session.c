#include "iot_provisioning_session.h"

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

static iot_prov_request_t valid_request(void)
{
    iot_prov_request_t request;

    memset(&request, 0, sizeof(request));
    snprintf(request.wifi_ssid, sizeof(request.wifi_ssid), "HomeWiFi");
    snprintf(request.wifi_password, sizeof(request.wifi_password),
             "password123");
    request.open_network = false;
    snprintf(request.mqtt_host, sizeof(request.mqtt_host),
             "mqtt.example.com");
    request.mqtt_port = 1883U;
    request.mqtt_tls = false;
    snprintf(request.mqtt_username, sizeof(request.mqtt_username),
             "gateway");
    snprintf(request.mqtt_password, sizeof(request.mqtt_password),
             "mqtt-secret");
    snprintf(request.gateway_id, sizeof(request.gateway_id), "gw-001");
    return request;
}

static void init_session(iot_prov_session_t *session)
{
    CHECK(iot_prov_session_init(session, "IoT-Home-3F9A", "0.1.0") ==
          IOT_PROV_OK);
}

static void start_passing_test(iot_prov_session_t *session,
                               const char *test_id, uint32_t now_ms)
{
    iot_prov_request_t request = valid_request();
    iot_prov_status_t status;

    CHECK(iot_prov_begin(session, &request, test_id, now_ms) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_wifi_result(session, test_id, now_ms + 1, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_mqtt_result(session, test_id, now_ms + 2, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_get_status(session, now_ms + 3, &status) ==
          IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TEST_PASSED);
    CHECK(status.can_commit);
}

static void test_initial_state(void)
{
    iot_prov_session_t session;
    iot_prov_status_t status;

    init_session(&session);
    CHECK(iot_prov_get_status(&session, 0U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_WAITING);
    CHECK(!status.can_commit);
    CHECK(status.input == IOT_PROV_STEP_IDLE);
    CHECK(status.wifi == IOT_PROV_STEP_IDLE);
    CHECK(status.mqtt == IOT_PROV_STEP_IDLE);
}

static void test_invalid_init(void)
{
    iot_prov_session_t session;

    CHECK(iot_prov_session_init(NULL, "IoT-Home", "0.1") ==
          IOT_PROV_ERR_INVALID_ARG);
    CHECK(iot_prov_session_init(&session, "", "0.1") ==
          IOT_PROV_ERR_INVALID_ARG);
    CHECK(iot_prov_session_init(&session, "IoT-Home", "") ==
          IOT_PROV_ERR_INVALID_ARG);
}

static void test_full_success_and_commit(void)
{
    iot_prov_session_t session;
    iot_prov_status_t status;
    iot_prov_bundle_t bundle;

    init_session(&session);
    start_passing_test(&session, "tmp-0001", 1000U);

    CHECK(iot_prov_prepare_commit(&session, "tmp-0001", 1000U, &bundle) ==
          IOT_PROV_OK);
    CHECK(strcmp(bundle.wifi_ssid, "HomeWiFi") == 0);
    CHECK(strcmp(bundle.wifi_password, "password123") == 0);
    CHECK(strcmp(bundle.mqtt_uri, "mqtt://mqtt.example.com:1883") == 0);
    CHECK(strcmp(bundle.mqtt_username, "gateway") == 0);
    CHECK(strcmp(bundle.mqtt_password, "mqtt-secret") == 0);
    CHECK(strcmp(bundle.mqtt_client_id, "gw-001") == 0);

    CHECK(iot_prov_commit_succeeded(&session, "tmp-0001", 1000U) ==
          IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 1000U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_COMMITTED);
    CHECK(!status.can_commit);
    CHECK(iot_prov_cancel(&session, "tmp-0001", 1000U) ==
          IOT_PROV_ERR_INVALID_STATE);
    CHECK(iot_prov_prepare_commit(&session, "tmp-0001", 1000U, &bundle) ==
          IOT_PROV_ERR_INVALID_STATE);
}

static void test_results_can_arrive_in_either_order(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request = valid_request();
    iot_prov_status_t status;

    init_session(&session);
    CHECK(iot_prov_begin(&session, &request, "tmp-order", 100U) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_mqtt_result(&session, "tmp-order", 101U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 102U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TESTING);
    CHECK(iot_prov_note_wifi_result(&session, "tmp-order", 103U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 104U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TEST_PASSED);
}

static void test_wifi_failure(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request = valid_request();
    iot_prov_status_t status;
    iot_prov_bundle_t bundle;

    init_session(&session);
    CHECK(iot_prov_begin(&session, &request, "tmp-wifi01", 200U) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_wifi_result(
              &session, "tmp-wifi01", 201U, false,
              IOT_PROV_ERR_WIFI_AUTH_FAILED,
              IOT_PROV_FIELD_WIFI_PASSWORD) == IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 202U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TEST_FAILED);
    CHECK(status.wifi == IOT_PROV_STEP_FAILED);
    CHECK(status.error_count == 1U);
    CHECK(status.errors[0].code == IOT_PROV_ERR_WIFI_AUTH_FAILED);
    CHECK(status.errors[0].field == IOT_PROV_FIELD_WIFI_PASSWORD);
    CHECK(iot_prov_prepare_commit(&session, "tmp-wifi01", 203U,
                                  &bundle) ==
          IOT_PROV_ERR_TEST_NOT_PASSED);
}

static void test_mqtt_failure(void)
{
    iot_prov_session_t session;
    iot_prov_status_t status;

    init_session(&session);
    start_passing_test(&session, "tmp-before", 100U);
    CHECK(iot_prov_cancel(&session, "tmp-before", 200U) == IOT_PROV_OK);

    start_passing_test(&session, "tmp-mqtt", 300U);
    CHECK(iot_prov_commit_failed(&session, "tmp-mqtt", 301U,
                                 IOT_PROV_ERR_MQTT_CONNECT_FAILED,
                                 IOT_PROV_FIELD_MQTT_HOST) == IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 302U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_ERROR);
    CHECK(status.error_count == 1U);
    CHECK(status.errors[0].code ==
          IOT_PROV_ERR_MQTT_CONNECT_FAILED);
    CHECK(iot_prov_cancel(&session, NULL, 303U) == IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 304U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_WAITING);
}

static void test_only_one_active_test(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request = valid_request();

    init_session(&session);
    CHECK(iot_prov_begin(&session, &request, "tmp-active1", 100U) ==
          IOT_PROV_OK);
    CHECK(iot_prov_begin(&session, &request, "tmp-active2", 101U) ==
          IOT_PROV_ERR_TEST_ACTIVE);
    CHECK(iot_prov_note_wifi_result(&session, "tmp-active2", 102U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_ERR_TEST_NOT_FOUND);
}

static void test_wrong_test_id(void)
{
    iot_prov_session_t session;
    iot_prov_bundle_t bundle;

    init_session(&session);
    start_passing_test(&session, "tmp-right", 100U);
    CHECK(iot_prov_prepare_commit(&session, "tmp-wrong", 101U, &bundle) ==
          IOT_PROV_ERR_TEST_NOT_FOUND);
    CHECK(iot_prov_commit_succeeded(&session, "tmp-wrong", 102U) ==
          IOT_PROV_ERR_TEST_NOT_FOUND);
    CHECK(iot_prov_cancel(&session, "tmp-wrong", 103U) ==
          IOT_PROV_ERR_TEST_NOT_FOUND);
}

static void test_invalid_requests(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request;

    init_session(&session);

    request = valid_request();
    request.wifi_ssid[0] = '\0';
    CHECK(iot_prov_begin(&session, & request, "tmp-bad", 0U) ==
          IOT_PROV_ERR_INVALID_FIELD);

    request = valid_request();
    snprintf(request.wifi_password, sizeof(request.wifi_password), "short");
    CHECK(iot_prov_begin(&session, & request, "tmp-bad", 60000U) ==
          IOT_PROV_ERR_INVALID_FIELD);

    request = valid_request();
    request.open_network = true;
    CHECK(iot_prov_begin(&session, & request, "tmp-bad", 120000U) ==
          IOT_PROV_ERR_INVALID_FIELD);

    request = valid_request();
    request.mqtt_tls = true;
    CHECK(iot_prov_begin(&session, & request, "tmp-bad", 180000U) ==
          IOT_PROV_ERR_TLS_NOT_CONFIGURED);

    request = valid_request();
    request.mqtt_port = 0U;
    CHECK(iot_prov_begin(&session, & request, "tmp-bad", 240000U) ==
          IOT_PROV_ERR_INVALID_FIELD);

    request = valid_request();
    snprintf(request.gateway_id, sizeof(request.gateway_id), "bad/id");
    CHECK(iot_prov_begin(&session, & request, "tmp-bad", 300000U) ==
          IOT_PROV_ERR_INVALID_FIELD);

    request = valid_request();
    snprintf(request.mqtt_host, sizeof(request.mqtt_host), "bad host");
    CHECK(iot_prov_begin(&session, & request, "tmp-bad", 360000U) ==
          IOT_PROV_ERR_INVALID_FIELD);

    request = valid_request();
    snprintf(request.mqtt_host, sizeof(request.mqtt_host),
             "example.com/path");
    CHECK(iot_prov_begin(&session, & request, "tmp-bad", 420000U) ==
          IOT_PROV_ERR_INVALID_FIELD);
}

static void test_testing_expiration(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request = valid_request();
    iot_prov_status_t status;

    init_session(&session);
    CHECK(iot_prov_begin(&session, &request, "tmp-expire", 1000U) ==
          IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 300999U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TESTING);
    CHECK(status.expires_at_ms == 301000U);

    CHECK(iot_prov_get_status(&session, 301000U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TEST_FAILED);
    CHECK(status.error_count == 1U);
    CHECK(status.errors[0].code == IOT_PROV_ERR_TEST_EXPIRED);
}

static void test_partial_testing_expiration(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request = valid_request();
    iot_prov_status_t status;

    init_session(&session);
    CHECK(iot_prov_begin(&session, &request, "tmp-partial", 1000U) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_wifi_result(&session, "tmp-partial", 1001U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 300999U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TESTING);
    CHECK(status.wifi == IOT_PROV_STEP_PASSED);
    CHECK(status.mqtt == IOT_PROV_STEP_PENDING);

    CHECK(iot_prov_get_status(&session, 301000U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TEST_FAILED);
    CHECK(status.wifi == IOT_PROV_STEP_PASSED);
    CHECK(status.mqtt == IOT_PROV_STEP_FAILED);
    CHECK(status.error_count == 1U);
    CHECK(status.errors[0].code == IOT_PROV_ERR_TEST_EXPIRED);
}

static void test_passed_expiration(void)
{
    iot_prov_session_t session;
    iot_prov_status_t status;
    iot_prov_bundle_t bundle;

    init_session(&session);
    start_passing_test(&session, "tmp-pass01", 1000U);
    CHECK(iot_prov_get_status(&session, 300999U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TEST_PASSED);

    CHECK(iot_prov_get_status(&session, 301000U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_WAITING);
    CHECK(!status.can_commit);
    CHECK(iot_prov_prepare_commit(&session, "tmp-pass01", 310001U,
                                  &bundle) ==
          IOT_PROV_ERR_TEST_EXPIRED);
}

static void test_cancel(void)
{
    iot_prov_session_t session;
    iot_prov_status_t status;

    init_session(&session);
    start_passing_test(&session, "tmp-cancel", 100U);
    CHECK(iot_prov_cancel(&session, "tmp-cancel", 200U) == IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 201U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_WAITING);
}

static void test_rate_limit(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request = valid_request();

    init_session(&session);
    for (uint8_t i = 0U; i < 5U; ++i) {
        char id[16];
        uint32_t now = 1000U + (uint32_t)i * 100U;

        snprintf(id, sizeof(id), "tmp-%u", (unsigned)(i + 1U));
        CHECK(iot_prov_begin(&session, &request, id, now) == IOT_PROV_OK);
        CHECK(iot_prov_cancel(&session, id, now + 1U) == IOT_PROV_OK);
    }

    CHECK(iot_prov_begin(&session, &request, "tmp-6", 1500U) ==
          IOT_PROV_ERR_RATE_LIMITED);
    CHECK(iot_prov_begin(&session, &request, "tmp-7", 61000U) ==
          IOT_PROV_OK);
}

static void test_snapshot_immutable(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request = valid_request();
    iot_prov_bundle_t bundle;

    init_session(&session);
    CHECK(iot_prov_begin(&session, &request, "tmp-immutable", 100U) ==
          IOT_PROV_OK);
    snprintf(request.wifi_ssid, sizeof(request.wifi_ssid), "ChangedWiFi");
    snprintf(request.mqtt_host, sizeof(request.mqtt_host),
             "changed.example.com");

    CHECK(iot_prov_note_wifi_result(&session, "tmp-immutable", 101U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_mqtt_result(&session, "tmp-immutable", 102U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_prepare_commit(&session, "tmp-immutable", 103U,
                                  &bundle) == IOT_PROV_OK);
    CHECK(strcmp(bundle.wifi_ssid, "HomeWiFi") == 0);
    CHECK(strcmp(bundle.mqtt_uri, "mqtt://mqtt.example.com:1883") == 0);
}

static void test_new_test_after_failure(void)
{
    iot_prov_session_t session;
    iot_prov_request_t request = valid_request();
    iot_prov_status_t status;

    init_session(&session);
    CHECK(iot_prov_begin(&session, &request, "tmp-old", 100U) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_wifi_result(&session, "tmp-old", 101U, false,
                                    IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT,
                                    IOT_PROV_FIELD_WIFI_SSID) ==
          IOT_PROV_OK);
    CHECK(iot_prov_begin(&session, &request, "tmp-new", 200U) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_wifi_result(&session, "tmp-old", 201U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_ERR_TEST_NOT_FOUND);
    CHECK(iot_prov_note_wifi_result(&session, "tmp-new", 202U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_note_mqtt_result(&session, "tmp-new", 203U, true,
                                    IOT_PROV_OK, IOT_PROV_FIELD_NONE) ==
          IOT_PROV_OK);
    CHECK(iot_prov_get_status(&session, 204U, &status) == IOT_PROV_OK);
    CHECK(status.state == IOT_PROV_STATE_TEST_PASSED);
}

int main(void)
{
    test_initial_state();
    test_invalid_init();
    test_full_success_and_commit();
    test_results_can_arrive_in_either_order();
    test_wifi_failure();
    test_mqtt_failure();
    test_only_one_active_test();
    test_wrong_test_id();
    test_invalid_requests();
    test_testing_expiration();
    test_partial_testing_expiration();
    test_passed_expiration();
    test_cancel();
    test_rate_limit();
    test_snapshot_immutable();
    test_new_test_after_failure();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all provisioning session checks passed\n");
    return 0;
}


