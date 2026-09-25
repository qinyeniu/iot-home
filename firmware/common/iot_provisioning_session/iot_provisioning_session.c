/*
 * Provisioning session core implementation.
 */

#include "iot_provisioning_session.h"

#include <stdio.h>
#include <string.h>

#include "iot_credentials_validation.h"
#include "iot_mqtt_uri_validation.h"

#define IOT_PROV_TEST_TTL_MS       300000u
#define IOT_PROV_RATE_WINDOW_MS    60000u
#define IOT_PROV_RATE_LIMIT        5u

static size_t bounded_strlen(const char *value, size_t capacity)
{
    size_t len = 0U;

    if (value == NULL) {
        return capacity;
    }
    while (len < capacity && value[len] != '\0') {
        ++len;
    }
    return len;
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size > 0U) {
        (void)snprintf(dst, dst_size, "%s", src == NULL ? "" : src);
    }
}

static bool same_test_id(const char *left, const char *right)
{
    return strcmp(left, right) == 0;
}

static void append_error_locked(iot_prov_session_t *session,
                                iot_prov_error_t code,
                                iot_prov_field_t field)
{
    if (session->error_count < IOT_PROV_ERROR_CAPACITY) {
        session->errors[session->error_count].code = code;
        session->errors[session->error_count].field = field;
        ++session->error_count;
    }
}

static void set_waiting_locked(iot_prov_session_t *session)
{
    session->state = IOT_PROV_STATE_WAITING;
    session->input = IOT_PROV_STEP_IDLE;
    session->wifi = IOT_PROV_STEP_IDLE;
    session->mqtt = IOT_PROV_STEP_IDLE;
    session->test_active = false;
    session->test_id[0] = '\0';
    session->started_at_ms = 0U;
    memset(&session->snapshot, 0, sizeof(session->snapshot));
    session->error_count = 0U;
}

static void fail_current_locked(iot_prov_session_t *session,
                                iot_prov_error_t code,
                                iot_prov_field_t field)
{
    session->state = IOT_PROV_STATE_TEST_FAILED;
    append_error_locked(session, code, field);
    session->test_active = false;
    session->test_id[0] = '\0';
    session->started_at_ms = 0U;
    memset(&session->snapshot, 0, sizeof(session->snapshot));
}

static bool label_is_valid(const char *value, size_t max_len)
{
    size_t len = bounded_strlen(value, max_len + 1U);

    return len > 0U && len <= max_len;
}

static bool validate_test_id(const char *test_id)
{
    size_t len;
    size_t i;

    if (test_id == NULL) {
        return false;
    }
    len = bounded_strlen(test_id, IOT_PROV_TEST_ID_MAX_LEN + 1U);
    if (len <= 4U || len > IOT_PROV_TEST_ID_MAX_LEN ||
        strncmp(test_id, "tmp-", 4U) != 0) {
        return false;
    }

    for (i = 4U; i < len; ++i) {
        char ch = test_id[i];

        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    return true;
}

static bool validate_gateway_id(const char *gateway_id)
{
    size_t len;
    size_t i;

    if (gateway_id == NULL) {
        return false;
    }
    len = bounded_strlen(gateway_id, 65U);
    if (len == 0U) {
        return true;
    }
    if (len > 64U) {
        return false;
    }

    for (i = 0U; i < len; ++i) {
        char ch = gateway_id[i];

        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static void validation_detail_set(iot_prov_error_info_t *detail,
                                   iot_prov_error_t code,
                                   iot_prov_field_t field)
{
    if (detail != NULL) {
        detail->code = code;
        detail->field = field;
    }
}

static iot_prov_error_t validate_request(
    const iot_prov_request_t *request,
    char *mqtt_uri,
    size_t mqtt_uri_size,
    iot_prov_error_info_t *detail)
{
    iot_credentials_field_t failed_field = IOT_CREDENTIALS_FIELD_NONE;
    iot_credentials_error_t credentials_error;
    iot_mqtt_uri_error_t uri_error;
    size_t wifi_password_len;
    int uri_len;

    credentials_error = iot_credentials_validate_text(
        request->wifi_ssid, sizeof(request->wifi_ssid),
        IOT_PROV_AP_SSID_MAX_LEN, true, false,
        IOT_CREDENTIALS_FIELD_WIFI_SSID, &failed_field);
    if (credentials_error != IOT_CREDENTIALS_OK) {
        validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                              IOT_PROV_FIELD_WIFI_SSID);
        return IOT_PROV_ERR_INVALID_FIELD;
    }

    wifi_password_len = bounded_strlen(request->wifi_password,
                                       sizeof(request->wifi_password));
    if (request->open_network) {
        if (wifi_password_len != 0U) {
            validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                                  IOT_PROV_FIELD_WIFI_PASSWORD);
            return IOT_PROV_ERR_INVALID_FIELD;
        }
    } else {
        if (wifi_password_len < 8U || wifi_password_len > 63U) {
            validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                                  IOT_PROV_FIELD_WIFI_PASSWORD);
            return IOT_PROV_ERR_INVALID_FIELD;
        }
        credentials_error = iot_credentials_validate_text(
            request->wifi_password, sizeof(request->wifi_password), 63U,
            true, false, IOT_CREDENTIALS_FIELD_WIFI_PASSWORD, &failed_field);
        if (credentials_error != IOT_CREDENTIALS_OK) {
            validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                                  IOT_PROV_FIELD_WIFI_PASSWORD);
            return IOT_PROV_ERR_INVALID_FIELD;
        }
    }

    if (request->mqtt_tls) {
        validation_detail_set(detail, IOT_PROV_ERR_TLS_NOT_CONFIGURED,
                              IOT_PROV_FIELD_MQTT_TLS);
        return IOT_PROV_ERR_TLS_NOT_CONFIGURED;
    }
    if (request->mqtt_port == 0U) {
        validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                              IOT_PROV_FIELD_MQTT_PORT);
        return IOT_PROV_ERR_INVALID_FIELD;
    }

    credentials_error = iot_credentials_validate_text(
        request->mqtt_host, sizeof(request->mqtt_host),
        IOT_PROV_MQTT_HOST_MAX_LEN, true, false,
        IOT_CREDENTIALS_FIELD_MQTT_URI, &failed_field);
    if (credentials_error != IOT_CREDENTIALS_OK) {
        validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                              IOT_PROV_FIELD_MQTT_HOST);
        return IOT_PROV_ERR_INVALID_FIELD;
    }

    credentials_error = iot_credentials_validate_text(
        request->mqtt_username, sizeof(request->mqtt_username), 64U,
        false, true, IOT_CREDENTIALS_FIELD_MQTT_USERNAME, &failed_field);
    if (credentials_error != IOT_CREDENTIALS_OK) {
        validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                              IOT_PROV_FIELD_MQTT_USERNAME);
        return IOT_PROV_ERR_INVALID_FIELD;
    }

    credentials_error = iot_credentials_validate_text(
        request->mqtt_password, sizeof(request->mqtt_password), 64U,
        false, true, IOT_CREDENTIALS_FIELD_MQTT_PASSWORD, &failed_field);
    if (credentials_error != IOT_CREDENTIALS_OK) {
        validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                              IOT_PROV_FIELD_MQTT_PASSWORD);
        return IOT_PROV_ERR_INVALID_FIELD;
    }

    if (!validate_gateway_id(request->gateway_id)) {
        validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                              IOT_PROV_FIELD_GATEWAY_ID);
        return IOT_PROV_ERR_INVALID_FIELD;
    }

    uri_len = snprintf(mqtt_uri, mqtt_uri_size, "mqtt://%s:%u",
                       request->mqtt_host, (unsigned)request->mqtt_port);
    if (uri_len < 0 || (size_t)uri_len >= mqtt_uri_size) {
        validation_detail_set(detail, IOT_PROV_ERR_INVALID_FIELD,
                              IOT_PROV_FIELD_MQTT_HOST);
        return IOT_PROV_ERR_INVALID_FIELD;
    }

    uri_error = iot_mqtt_uri_validate(mqtt_uri, mqtt_uri_size);
    if (uri_error == IOT_MQTT_URI_OK) {
        return IOT_PROV_OK;
    }
    validation_detail_set(detail,
                          IOT_PROV_ERR_INVALID_FIELD,
                          uri_error == IOT_MQTT_URI_ERR_PORT
                              ? IOT_PROV_FIELD_MQTT_PORT
                              : IOT_PROV_FIELD_MQTT_HOST);
    return IOT_PROV_ERR_INVALID_FIELD;
}

static iot_prov_error_t rate_limit_note_locked(iot_prov_session_t *session,
                                               uint32_t now_ms)
{
    uint8_t recent = 0U;
    uint8_t i;

    for (i = 0U; i < session->request_count; ++i) {
        if (now_ms - session->request_times_ms[i] <
                IOT_PROV_RATE_WINDOW_MS) {
            ++recent;
        }
    }

    if (recent >= IOT_PROV_RATE_LIMIT) {
        return IOT_PROV_ERR_RATE_LIMITED;
    }

    if (session->request_count < IOT_PROV_RATE_LIMIT) {
        session->request_times_ms[session->request_count] = now_ms;
        ++session->request_count;
    } else {
        session->request_times_ms[session->request_ring_index] = now_ms;
        session->request_ring_index =
            (uint8_t)((session->request_ring_index + 1U) %
                      IOT_PROV_RATE_LIMIT);
    }
    return IOT_PROV_OK;
}

static void expire_if_due_locked(iot_prov_session_t *session,
                                 uint32_t now_ms)
{
    if (!session->test_active) {
        return;
    }
    if (now_ms - session->started_at_ms < IOT_PROV_TEST_TTL_MS) {
        return;
    }

    if (session->state == IOT_PROV_STATE_TESTING) {
        if (session->wifi == IOT_PROV_STEP_PENDING) {
            session->wifi = IOT_PROV_STEP_FAILED;
        }
        if (session->mqtt == IOT_PROV_STEP_PENDING) {
            session->mqtt = IOT_PROV_STEP_FAILED;
        }
        fail_current_locked(session, IOT_PROV_ERR_TEST_EXPIRED,
                            IOT_PROV_FIELD_NONE);
    } else if (session->state == IOT_PROV_STATE_TEST_PASSED) {
        set_waiting_locked(session);
    }

}
iot_prov_error_t iot_prov_session_init(
    iot_prov_session_t *session,
    const char *ap_ssid,
    const char *firmware_version)
{
    if (session == NULL ||
        !label_is_valid(ap_ssid, IOT_PROV_AP_SSID_MAX_LEN) ||
        !label_is_valid(firmware_version,
                        IOT_PROV_FIRMWARE_VERSION_MAX_LEN)) {
        return IOT_PROV_ERR_INVALID_ARG;
    }

    memset(session, 0, sizeof(*session));
    copy_text(session->ap_ssid, sizeof(session->ap_ssid), ap_ssid);
    copy_text(session->firmware_version, sizeof(session->firmware_version),
              firmware_version);
    session->initialized = true;
    session->state = IOT_PROV_STATE_WAITING;
    session->input = IOT_PROV_STEP_IDLE;
    session->wifi = IOT_PROV_STEP_IDLE;
    session->mqtt = IOT_PROV_STEP_IDLE;
    return IOT_PROV_OK;
}

static iot_prov_error_t begin_impl(
    iot_prov_session_t *session,
    const iot_prov_request_t *request,
    const char *test_id,
    uint32_t now_ms,
    bool rate_limit_already_noted,
    iot_prov_error_info_t *error_detail)
{
    char mqtt_uri[161];
    iot_prov_error_t error;

    if (error_detail != NULL) {
        error_detail->code = IOT_PROV_OK;
        error_detail->field = IOT_PROV_FIELD_NONE;
    }
    if (session == NULL || !session->initialized || request == NULL) {
        validation_detail_set(error_detail, IOT_PROV_ERR_INVALID_ARG,
                              IOT_PROV_FIELD_NONE);
        return IOT_PROV_ERR_INVALID_ARG;
    }

    if (!rate_limit_already_noted) {
        error = rate_limit_note_locked(session, now_ms);
        if (error != IOT_PROV_OK) {
            validation_detail_set(error_detail, error, IOT_PROV_FIELD_NONE);
            return error;
        }
    }

    if (session->state == IOT_PROV_STATE_TESTING ||
        session->state == IOT_PROV_STATE_TEST_PASSED) {
        validation_detail_set(error_detail, IOT_PROV_ERR_TEST_ACTIVE,
                              IOT_PROV_FIELD_NONE);
        return IOT_PROV_ERR_TEST_ACTIVE;
    }
    if (session->state == IOT_PROV_STATE_COMMITTED ||
        session->state == IOT_PROV_STATE_ERROR) {
        validation_detail_set(error_detail, IOT_PROV_ERR_INVALID_STATE,
                              IOT_PROV_FIELD_NONE);
        return IOT_PROV_ERR_INVALID_STATE;
    }
    if (!validate_test_id(test_id)) {
        validation_detail_set(error_detail, IOT_PROV_ERR_INVALID_ARG,
                              IOT_PROV_FIELD_NONE);
        return IOT_PROV_ERR_INVALID_ARG;
    }

    error = validate_request(request, mqtt_uri, sizeof(mqtt_uri),
                            error_detail);
    if (error != IOT_PROV_OK) {
        return error;
    }

    set_waiting_locked(session);
    session->state = IOT_PROV_STATE_TESTING;
    session->input = IOT_PROV_STEP_PASSED;
    session->wifi = IOT_PROV_STEP_PENDING;
    session->mqtt = IOT_PROV_STEP_PENDING;
    session->test_active = true;
    copy_text(session->test_id, sizeof(session->test_id), test_id);
    session->started_at_ms = now_ms;

    copy_text(session->snapshot.wifi_ssid,
              sizeof(session->snapshot.wifi_ssid), request->wifi_ssid);
    copy_text(session->snapshot.wifi_password,
              sizeof(session->snapshot.wifi_password), request->wifi_password);
    copy_text(session->snapshot.mqtt_uri,
              sizeof(session->snapshot.mqtt_uri), mqtt_uri);
    copy_text(session->snapshot.mqtt_username,
              sizeof(session->snapshot.mqtt_username),
              request->mqtt_username);
    copy_text(session->snapshot.mqtt_password,
              sizeof(session->snapshot.mqtt_password),
              request->mqtt_password);
    copy_text(session->snapshot.mqtt_client_id,
              sizeof(session->snapshot.mqtt_client_id),
              request->gateway_id);
    return IOT_PROV_OK;
}

iot_prov_error_t iot_prov_note_test_rate_limit(
    iot_prov_session_t *session,
    uint32_t now_ms)
{
    if (session == NULL || !session->initialized) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    return rate_limit_note_locked(session, now_ms);
}

iot_prov_error_t iot_prov_begin_ex(
    iot_prov_session_t *session,
    const iot_prov_request_t *request,
    const char *test_id,
    uint32_t now_ms,
    iot_prov_error_info_t *error_detail)
{
    return begin_impl(session, request, test_id, now_ms, false, error_detail);
}

iot_prov_error_t iot_prov_begin_after_rate_limit_ex(
    iot_prov_session_t *session,
    const iot_prov_request_t *request,
    const char *test_id,
    uint32_t now_ms,
    iot_prov_error_info_t *error_detail)
{
    return begin_impl(session, request, test_id, now_ms, true, error_detail);
}
iot_prov_error_t iot_prov_begin(
    iot_prov_session_t *session,
    const iot_prov_request_t *request,
    const char *test_id,
    uint32_t now_ms)
{
    return iot_prov_begin_ex(session, request, test_id, now_ms, NULL);
}

iot_prov_error_t iot_prov_note_wifi_result(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms,
    bool passed,
    iot_prov_error_t error_code,
    iot_prov_field_t error_field)
{
    if (session == NULL || !session->initialized || test_id == NULL) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    expire_if_due_locked(session, now_ms);
    if (!session->test_active ||
        session->state != IOT_PROV_STATE_TESTING ||
        !same_test_id(session->test_id, test_id)) {
        return IOT_PROV_ERR_TEST_NOT_FOUND;
    }

    if (!passed) {
        session->wifi = IOT_PROV_STEP_FAILED;
        fail_current_locked(session,
                            error_code == IOT_PROV_OK ? IOT_PROV_ERR_INTERNAL
                                                      : error_code,
                            error_field);
        return IOT_PROV_OK;
    }

    session->wifi = IOT_PROV_STEP_PASSED;
    if (session->mqtt == IOT_PROV_STEP_PASSED) {
        session->state = IOT_PROV_STATE_TEST_PASSED;
        session->error_count = 0U;
    }
    return IOT_PROV_OK;
}

iot_prov_error_t iot_prov_note_mqtt_result(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms,
    bool passed,
    iot_prov_error_t error_code,
    iot_prov_field_t error_field)
{
    if (session == NULL || !session->initialized || test_id == NULL) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    expire_if_due_locked(session, now_ms);
    if (!session->test_active ||
        session->state != IOT_PROV_STATE_TESTING ||
        !same_test_id(session->test_id, test_id)) {
        return IOT_PROV_ERR_TEST_NOT_FOUND;
    }

    if (!passed) {
        session->mqtt = IOT_PROV_STEP_FAILED;
        fail_current_locked(session,
                            error_code == IOT_PROV_OK ? IOT_PROV_ERR_INTERNAL
                                                      : error_code,
                            error_field);
        return IOT_PROV_OK;
    }

    session->mqtt = IOT_PROV_STEP_PASSED;
    if (session->wifi == IOT_PROV_STEP_PASSED) {
        session->state = IOT_PROV_STATE_TEST_PASSED;
        session->error_count = 0U;
    }
    return IOT_PROV_OK;
}

iot_prov_error_t iot_prov_prepare_commit(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms,
    iot_prov_bundle_t *bundle)
{
    if (session == NULL || !session->initialized || test_id == NULL ||
        bundle == NULL) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    expire_if_due_locked(session, now_ms);
    if (!session->test_active) {
        if (session->state == IOT_PROV_STATE_TEST_FAILED) {
            return IOT_PROV_ERR_TEST_NOT_PASSED;
        }
        if (session->state == IOT_PROV_STATE_ERROR ||
            session->state == IOT_PROV_STATE_COMMITTED) {
            return IOT_PROV_ERR_INVALID_STATE;
        }
        return IOT_PROV_ERR_TEST_EXPIRED;
    }
    if (!same_test_id(session->test_id, test_id)) {
        return IOT_PROV_ERR_TEST_NOT_FOUND;
    }
    if (session->state != IOT_PROV_STATE_TEST_PASSED) {
        return IOT_PROV_ERR_TEST_NOT_PASSED;
    }

    *bundle = session->snapshot;
    return IOT_PROV_OK;
}

iot_prov_error_t iot_prov_commit_succeeded(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms)
{
    if (session == NULL || !session->initialized || test_id == NULL) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    expire_if_due_locked(session, now_ms);
    if (!session->test_active ||
        !same_test_id(session->test_id, test_id)) {
        return IOT_PROV_ERR_TEST_NOT_FOUND;
    }
    if (session->state != IOT_PROV_STATE_TEST_PASSED) {
        return IOT_PROV_ERR_TEST_NOT_PASSED;
    }

    session->state = IOT_PROV_STATE_COMMITTED;
    session->test_active = false;
    session->test_id[0] = '\0';
    session->started_at_ms = 0U;
    memset(&session->snapshot, 0, sizeof(session->snapshot));
    return IOT_PROV_OK;
}

iot_prov_error_t iot_prov_commit_failed(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms,
    iot_prov_error_t error_code,
    iot_prov_field_t error_field)
{
    if (session == NULL || !session->initialized || test_id == NULL) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    expire_if_due_locked(session, now_ms);
    if (!session->test_active ||
        !same_test_id(session->test_id, test_id)) {
        return IOT_PROV_ERR_TEST_NOT_FOUND;
    }
    if (session->state != IOT_PROV_STATE_TEST_PASSED) {
        return IOT_PROV_ERR_TEST_NOT_PASSED;
    }

    session->state = IOT_PROV_STATE_ERROR;
    append_error_locked(session,
                        error_code == IOT_PROV_OK ? IOT_PROV_ERR_INTERNAL
                                                  : error_code,
                        error_field);
    session->test_active = false;
    session->test_id[0] = '\0';
    session->started_at_ms = 0U;
    memset(&session->snapshot, 0, sizeof(session->snapshot));
    return IOT_PROV_OK;
}

iot_prov_error_t iot_prov_cancel(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms)
{
    if (session == NULL || !session->initialized) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    expire_if_due_locked(session, now_ms);
    if (session->state == IOT_PROV_STATE_COMMITTED) {
        return IOT_PROV_ERR_INVALID_STATE;
    }

    if (session->test_active) {
        if (test_id != NULL &&
            !same_test_id(session->test_id, test_id)) {
            return IOT_PROV_ERR_TEST_NOT_FOUND;
        }
    }

    set_waiting_locked(session);
    return IOT_PROV_OK;
}

iot_prov_error_t iot_prov_get_status(
    iot_prov_session_t *session,
    uint32_t now_ms,
    iot_prov_status_t *status)
{
    uint8_t i;

    if (session == NULL || !session->initialized || status == NULL) {
        return IOT_PROV_ERR_INVALID_ARG;
    }

    expire_if_due_locked(session, now_ms);
    memset(status, 0, sizeof(*status));
    status->state = session->state;
    status->can_commit = session->test_active &&
                         session->state == IOT_PROV_STATE_TEST_PASSED;
    status->started_at_ms = session->started_at_ms;
    status->expires_at_ms = session->started_at_ms + IOT_PROV_TEST_TTL_MS;
    status->input = session->input;
    status->wifi = session->wifi;
    status->mqtt = session->mqtt;
    status->error_count = session->error_count;

    for (i = 0U; i < session->error_count; ++i) {
        status->errors[i] = session->errors[i];
    }
    return IOT_PROV_OK;
}





