/*
 * SoftAP provisioning HTTP adapter implementation.
 */

#include "iot_provisioning_http.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"

#define STATUS_PATH   "/api/provision/status"
#define TEST_PATH     "/api/provision/test"
#define COMMIT_PATH   "/api/provision/commit"
#define CANCEL_PATH   "/api/provision/cancel"

static const char *state_name(iot_prov_state_t state)
{
    switch (state) {
        case IOT_PROV_STATE_WAITING:
            return "waiting";
        case IOT_PROV_STATE_TESTING:
            return "testing";
        case IOT_PROV_STATE_TEST_PASSED:
            return "test_passed";
        case IOT_PROV_STATE_TEST_FAILED:
            return "test_failed";
        case IOT_PROV_STATE_COMMITTED:
            return "committed";
        case IOT_PROV_STATE_ERROR:
            return "error";
    }
    return "error";
}

static const char *step_name(iot_prov_step_t step)
{
    switch (step) {
        case IOT_PROV_STEP_IDLE:
            return "idle";
        case IOT_PROV_STEP_PENDING:
            return "pending";
        case IOT_PROV_STEP_PASSED:
            return "passed";
        case IOT_PROV_STEP_FAILED:
            return "failed";
    }
    return "idle";
}

static const char *field_name(iot_prov_field_t field)
{
    switch (field) {
        case IOT_PROV_FIELD_WIFI_SSID:
            return "wifi_ssid";
        case IOT_PROV_FIELD_WIFI_PASSWORD:
            return "wifi_password";
        case IOT_PROV_FIELD_OPEN_NETWORK:
            return "open_network";
        case IOT_PROV_FIELD_MQTT_HOST:
            return "mqtt_host";
        case IOT_PROV_FIELD_MQTT_PORT:
            return "mqtt_port";
        case IOT_PROV_FIELD_MQTT_TLS:
            return "mqtt_tls";
        case IOT_PROV_FIELD_MQTT_USERNAME:
            return "mqtt_username";
        case IOT_PROV_FIELD_MQTT_PASSWORD:
            return "mqtt_password";
        case IOT_PROV_FIELD_GATEWAY_ID:
            return "gateway_id";
        case IOT_PROV_FIELD_NONE:
            break;
    }
    return "";
}

static const char *error_name(iot_prov_error_t code)
{
    switch (code) {
        case IOT_PROV_ERR_INVALID_ARG:
            return "INVALID_ARG";
        case IOT_PROV_ERR_INVALID_STATE:
            return "INVALID_STATE";
        case IOT_PROV_ERR_INVALID_FIELD:
            return "INVALID_FIELD";
        case IOT_PROV_ERR_WIFI_AUTH_FAILED:
            return "WIFI_AUTH_FAILED";
        case IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT:
            return "WIFI_ASSOC_TIMEOUT";
        case IOT_PROV_ERR_WIFI_NO_IP_ADDRESS:
            return "WIFI_NO_IP_ADDRESS";
        case IOT_PROV_ERR_DNS_FAILED:
            return "DNS_FAILED";
        case IOT_PROV_ERR_MQTT_CONNECT_FAILED:
            return "MQTT_CONNECT_FAILED";
        case IOT_PROV_ERR_TLS_NOT_CONFIGURED:
            return "TLS_NOT_CONFIGURED";
        case IOT_PROV_ERR_RATE_LIMITED:
            return "RATE_LIMITED";
        case IOT_PROV_ERR_TEST_ACTIVE:
            return "TEST_ACTIVE";
        case IOT_PROV_ERR_TEST_NOT_FOUND:
            return "TEST_NOT_FOUND";
        case IOT_PROV_ERR_TEST_EXPIRED:
            return "TEST_EXPIRED";
        case IOT_PROV_ERR_TEST_NOT_PASSED:
            return "TEST_NOT_PASSED";
        case IOT_PROV_ERR_INTERNAL:
            return "INTERNAL_ERROR";
        case IOT_PROV_OK:
            break;
    }
    return "INTERNAL_ERROR";
}

static void response_clear(iot_http_response_t *response)
{
    memset(response, 0, sizeof(*response));
    response->content_type = "application/json";
}

static bool send_json(iot_http_response_t *response, uint16_t status_code,
                      cJSON *root)
{
    bool printed;

    response->status_code = status_code;
    printed = cJSON_PrintPreallocated(
        root, response->body, (int)sizeof(response->body), 0);
    cJSON_Delete(root);
    if (!printed) {
        response->status_code = 500;
        (void)snprintf(response->body, sizeof(response->body),
                       "{\"state\":\"error\",\"errors\":["
                       "{\"code\":\"INTERNAL_ERROR\",\"field\":\"\"}]}");
    }
    response->body_len = strlen(response->body);
    return printed;
}

static void add_error(cJSON *errors, iot_prov_error_t code,
                      iot_prov_field_t field)
{
    cJSON *item = cJSON_CreateObject();

    cJSON_AddStringToObject(item, "code", error_name(code));
    cJSON_AddStringToObject(item, "field", field_name(field));
    cJSON_AddItemToArray(errors, item);
}

static cJSON *named_error_body(const char *state, iot_prov_error_t code,
                               iot_prov_field_t field)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *errors = cJSON_AddArrayToObject(root, "errors");

    cJSON_AddStringToObject(root, "state", state);
    add_error(errors, code, field);
    return root;
}

static cJSON *error_body(iot_prov_error_t code, iot_prov_field_t field)
{
    return named_error_body("error", code, field);
}

static cJSON *test_error_body(iot_prov_error_t code, iot_prov_field_t field)
{
    return named_error_body("test_failed", code, field);
}

static cJSON *status_body(iot_http_adapter_t *adapter,
                          const iot_prov_status_t *status)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *tests = cJSON_AddObjectToObject(root, "tests");
    cJSON *errors = cJSON_AddArrayToObject(root, "errors");

    cJSON_AddStringToObject(root, "state", state_name(status->state));
    cJSON_AddStringToObject(root, "ap_ssid", adapter->session.ap_ssid);
    cJSON_AddStringToObject(root, "firmware_version",
                            adapter->session.firmware_version);
    cJSON_AddBoolToObject(root, "can_commit", status->can_commit);
    if (status->started_at_ms != 0U) {
        cJSON_AddNumberToObject(root, "test_expires_at",
                                (double)status->expires_at_ms);
    } else {
        cJSON_AddNullToObject(root, "test_expires_at");
    }

    cJSON_AddStringToObject(tests, "input", step_name(status->input));
    cJSON_AddStringToObject(tests, "wifi", step_name(status->wifi));
    cJSON_AddStringToObject(tests, "mqtt", step_name(status->mqtt));

    for (uint8_t i = 0U; i < status->error_count; ++i) {
        add_error(errors, status->errors[i].code, status->errors[i].field);
    }
    return root;
}

static iot_prov_error_t build_current_status(
    iot_http_adapter_t *adapter,
    uint32_t now_ms,
    iot_http_response_t *response,
    uint16_t status_code)
{
    iot_prov_status_t status;
    iot_prov_error_t error = iot_prov_get_status(
        &adapter->session, now_ms, &status);

    if (error != IOT_PROV_OK) {
        return error;
    }
    (void)send_json(response, status_code, status_body(adapter, &status));
    return IOT_PROV_OK;
}

static bool is_json_content_type(const char *content_type)
{
    if (content_type == NULL) {
        return false;
    }
    if (strcmp(content_type, "application/json") == 0) {
        return true;
    }
    return strncmp(content_type, "application/json;", 17) == 0;
}

static void clear_parse_scratch(iot_http_adapter_t *adapter)
{
    memset(adapter->parse_scratch, 0, sizeof(adapter->parse_scratch));
}

static void release_json(iot_http_adapter_t *adapter, cJSON *root)
{
    cJSON_Delete(root);
    clear_parse_scratch(adapter);
}

static cJSON *parse_json_body(iot_http_adapter_t *adapter,
                              const iot_http_request_t *request)
{
    clear_parse_scratch(adapter);
    if (request->body_len > IOT_HTTP_MAX_BODY) {
        return NULL;
    }
    if (request->body_len > 0U) {
        if (!is_json_content_type(request->content_type)) {
            return NULL;
        }
        memcpy(adapter->parse_scratch, request->body, request->body_len);
        adapter->parse_scratch[request->body_len] = '\0';
    } else {
        adapter->parse_scratch[0] = '\0';
    }
    {
        cJSON *root = cJSON_Parse(adapter->parse_scratch);

        if (root == NULL) {
            clear_parse_scratch(adapter);
        }
        return root;
    }
}

static bool copy_json_string(cJSON *root, const char *key, char *out,
                              size_t out_size, bool required,
                              iot_prov_field_t field,
                              iot_prov_error_info_t *detail)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    size_t len;

    if (item == NULL || cJSON_IsNull(item)) {
        if (required) {
            detail->code = IOT_PROV_ERR_INVALID_FIELD;
            detail->field = field;
            return false;
        }
        out[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(item)) {
        detail->code = IOT_PROV_ERR_INVALID_FIELD;
        detail->field = field;
        return false;
    }

    len = strlen(item->valuestring);
    if (len >= out_size) {
        detail->code = IOT_PROV_ERR_INVALID_FIELD;
        detail->field = field;
        return false;
    }
    memcpy(out, item->valuestring, len + 1U);
    return true;
}

static bool copy_json_bool(cJSON *root, const char *key, bool *out,
                           iot_prov_field_t field,
                           iot_prov_error_info_t *detail)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (item == NULL) {
        *out = false;
        return true;
    }
    if (cJSON_IsTrue(item)) {
        *out = true;
        return true;
    }
    if (cJSON_IsFalse(item)) {
        *out = false;
        return true;
    }
    detail->code = IOT_PROV_ERR_INVALID_FIELD;
    detail->field = field;
    return false;
}

static uint16_t status_for_begin_error(iot_prov_error_t error)
{
    switch (error) {
        case IOT_PROV_ERR_RATE_LIMITED:
            return 429;
        case IOT_PROV_ERR_TEST_ACTIVE:
        case IOT_PROV_ERR_INVALID_STATE:
        case IOT_PROV_ERR_TEST_EXPIRED:
        case IOT_PROV_ERR_TEST_NOT_PASSED:
            return 409;
        case IOT_PROV_ERR_INVALID_FIELD:
        case IOT_PROV_ERR_TLS_NOT_CONFIGURED:
            return 400;
        case IOT_PROV_ERR_TEST_NOT_FOUND:
            return 404;
        default:
            return 500;
    }
}

static iot_prov_error_t handle_status(
    iot_http_adapter_t *adapter,
    const iot_http_request_t *request,
    iot_http_response_t *response)
{
    return build_current_status(adapter, request->now_ms, response, 200);
}

static iot_prov_error_t handle_test(
    iot_http_adapter_t *adapter,
    const iot_http_request_t *request,
    iot_http_response_t *response)
{
    cJSON *root;
    iot_prov_request_t session_request;
    iot_prov_error_info_t detail = {
        .code = IOT_PROV_OK,
        .field = IOT_PROV_FIELD_NONE,
    };
    char test_id[33];
    iot_prov_error_t error;
    cJSON *port_item;

    response_clear(response);
    error = iot_prov_note_test_rate_limit(&adapter->session, request->now_ms);
    if (error == IOT_PROV_ERR_RATE_LIMITED) {
        (void)send_json(response, 429,
                        error_body(error, IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (error != IOT_PROV_OK) {
        (void)send_json(response, 500,
                        error_body(error, IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }

    if (request->body_len > IOT_HTTP_MAX_BODY) {
        (void)send_json(response, 413,
                        error_body(IOT_PROV_ERR_INVALID_FIELD,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (request->body_len > 0U &&
        !is_json_content_type(request->content_type)) {
        (void)send_json(response, 415,
                        error_body(IOT_PROV_ERR_INVALID_STATE,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }

    root = parse_json_body(adapter, request);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root != NULL) {
            release_json(adapter, root);
        }
        (void)send_json(response, 400,
                        error_body(IOT_PROV_ERR_INVALID_ARG,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }

    memset(&session_request, 0, sizeof(session_request));
    if (!copy_json_string(root, "wifi_ssid", session_request.wifi_ssid,
                          sizeof(session_request.wifi_ssid), true,
                          IOT_PROV_FIELD_WIFI_SSID, &detail) ||
        !copy_json_string(root, "wifi_password",
                          session_request.wifi_password,
                          sizeof(session_request.wifi_password), false,
                          IOT_PROV_FIELD_WIFI_PASSWORD, &detail) ||
        !copy_json_bool(root, "open_network",
                        &session_request.open_network,
                        IOT_PROV_FIELD_OPEN_NETWORK, &detail) ||
        !copy_json_string(root, "mqtt_host", session_request.mqtt_host,
                          sizeof(session_request.mqtt_host), true,
                          IOT_PROV_FIELD_MQTT_HOST, &detail) ||
        !copy_json_string(root, "mqtt_username",
                          session_request.mqtt_username,
                          sizeof(session_request.mqtt_username), false,
                          IOT_PROV_FIELD_MQTT_USERNAME, &detail) ||
        !copy_json_string(root, "mqtt_password",
                          session_request.mqtt_password,
                          sizeof(session_request.mqtt_password), false,
                          IOT_PROV_FIELD_MQTT_PASSWORD, &detail) ||
        !copy_json_string(root, "gateway_id", session_request.gateway_id,
                          sizeof(session_request.gateway_id), false,
                          IOT_PROV_FIELD_GATEWAY_ID, &detail)) {
        release_json(adapter, root);
        (void)send_json(response, 400, test_error_body(detail.code, detail.field));
        return IOT_PROV_OK;
    }

    port_item = cJSON_GetObjectItemCaseSensitive(root, "mqtt_port");
    if (port_item == NULL || !cJSON_IsNumber(port_item) ||
        port_item->valuedouble < 1.0 ||
        port_item->valuedouble > 65535.0 ||
        port_item->valuedouble != (double)(int)port_item->valuedouble) {
        release_json(adapter, root);
        (void)send_json(response, 400,
                        test_error_body(IOT_PROV_ERR_INVALID_FIELD,
                                        IOT_PROV_FIELD_MQTT_PORT));
        return IOT_PROV_OK;
    }
    session_request.mqtt_port =
        (uint16_t)(int)port_item->valuedouble;

    if (!copy_json_bool(root, "mqtt_tls", &session_request.mqtt_tls,
                        IOT_PROV_FIELD_MQTT_TLS, &detail)) {
        release_json(adapter, root);
        (void)send_json(response, 400, test_error_body(detail.code, detail.field));
        return IOT_PROV_OK;
    }
    release_json(adapter, root);

    if (!adapter->test_id_generator(test_id, sizeof(test_id),
                                    adapter->generator_context)) {
        memset(&session_request, 0, sizeof(session_request));
        (void)send_json(response, 500,
                        error_body(IOT_PROV_ERR_INTERNAL,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }

    error = iot_prov_begin_after_rate_limit_ex(&adapter->session, &session_request, test_id,
                              request->now_ms, &detail);
    if (error != IOT_PROV_OK) {
        memset(&session_request, 0, sizeof(session_request));
        (void)send_json(response, status_for_begin_error(error),
                        error_body(detail.code, detail.field));
        return IOT_PROV_OK;
    }
    memset(&session_request, 0, sizeof(session_request));

    {
        cJSON *accepted = cJSON_CreateObject();

        cJSON_AddStringToObject(accepted, "test_id", test_id);
        cJSON_AddStringToObject(accepted, "state", "testing");
        (void)send_json(response, 202, accepted);
    }
    return IOT_PROV_OK;
}

static uint16_t status_for_commit_error(iot_prov_error_t error)
{
    switch (error) {
        case IOT_PROV_ERR_TEST_NOT_FOUND:
            return 404;
        case IOT_PROV_ERR_INVALID_STATE:
        case IOT_PROV_ERR_TEST_EXPIRED:
        case IOT_PROV_ERR_TEST_NOT_PASSED:
            return 409;
        default:
            return 500;
    }
}

static iot_prov_error_t handle_commit(
    iot_http_adapter_t *adapter,
    const iot_http_request_t *request,
    iot_http_response_t *response)
{
    cJSON *root;
    iot_prov_bundle_t bundle;
    iot_prov_error_t error;
    char test_id[33];
    iot_prov_error_info_t parse_detail = {
        .code = IOT_PROV_ERR_INVALID_FIELD,
        .field = IOT_PROV_FIELD_NONE,
    };

    response_clear(response);
    if (request->body_len > IOT_HTTP_MAX_BODY) {
        (void)send_json(response, 413,
                        error_body(IOT_PROV_ERR_INVALID_FIELD,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (!is_json_content_type(request->content_type)) {
        (void)send_json(response, 415,
                        error_body(IOT_PROV_ERR_INVALID_STATE,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }

    root = parse_json_body(adapter, request);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root != NULL) {
            release_json(adapter, root);
        }
        (void)send_json(response, 400,
                        error_body(IOT_PROV_ERR_INVALID_ARG,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }

    if (!copy_json_string(root, "test_id", test_id, sizeof(test_id), true,
                          IOT_PROV_FIELD_NONE, &parse_detail)) {
        release_json(adapter, root);
        (void)send_json(response, 400,
                        error_body(IOT_PROV_ERR_INVALID_ARG,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    release_json(adapter, root);

    error = iot_prov_prepare_commit(&adapter->session, test_id,
                                    request->now_ms, &bundle);
    if (error != IOT_PROV_OK) {
        memset(&bundle, 0, sizeof(bundle));
        (void)send_json(response, status_for_commit_error(error),
                        error_body(error, IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (adapter->commit_handler == NULL) {
        memset(&bundle, 0, sizeof(bundle));
        (void)send_json(response, 501,
                        named_error_body("error", IOT_PROV_ERR_INVALID_STATE,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }

    {
        iot_http_commit_result_t result = adapter->commit_handler(
            &bundle, adapter->commit_context);

        if (result.code != IOT_PROV_OK) {
            (void)iot_prov_commit_failed(&adapter->session, test_id,
                                         request->now_ms, result.code,
                                         result.field);
            memset(&bundle, 0, sizeof(bundle));
            (void)send_json(response, 500,
                            named_error_body("error", result.code,
                                                result.field));
            return IOT_PROV_OK;
        }
    }

    error = iot_prov_commit_succeeded(&adapter->session, test_id,
                                      request->now_ms);
    if (error != IOT_PROV_OK) {
        memset(&bundle, 0, sizeof(bundle));
        (void)send_json(response, 500,
                        error_body(error, IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    memset(&bundle, 0, sizeof(bundle));
    return build_current_status(adapter, request->now_ms, response, 200);
}

static iot_prov_error_t handle_cancel(
    iot_http_adapter_t *adapter,
    const iot_http_request_t *request,
    iot_http_response_t *response)
{
    cJSON *root = NULL;
    char test_id[33];
    const char *test_id_arg = NULL;
    iot_prov_error_t error;
    iot_prov_error_info_t detail = {
        .code = IOT_PROV_ERR_INVALID_ARG,
        .field = IOT_PROV_FIELD_NONE,
    };

    response_clear(response);
    if (request->body_len > IOT_HTTP_MAX_BODY) {
        (void)send_json(response, 413,
                        error_body(IOT_PROV_ERR_INVALID_FIELD,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (request->body_len > 0U) {
        if (!is_json_content_type(request->content_type)) {
            (void)send_json(response, 415,
                            error_body(IOT_PROV_ERR_INVALID_STATE,
                                       IOT_PROV_FIELD_NONE));
            return IOT_PROV_OK;
        }
        root = parse_json_body(adapter, request);
        if (root == NULL || !cJSON_IsObject(root)) {
            if (root != NULL) {
                release_json(adapter, root);
            }
            (void)send_json(response, 400,
                            error_body(IOT_PROV_ERR_INVALID_ARG,
                                       IOT_PROV_FIELD_NONE));
            return IOT_PROV_OK;
        }
        if (!copy_json_string(root, "test_id", test_id, sizeof(test_id), false,
                              IOT_PROV_FIELD_NONE, &detail)) {
            release_json(adapter, root);
            (void)send_json(response, 400,
                            error_body(detail.code, detail.field));
            return IOT_PROV_OK;
        }
        test_id_arg = test_id[0] != '\0' ? test_id : NULL;
        release_json(adapter, root);
    }

    error = iot_prov_cancel(&adapter->session, test_id_arg,
                            request->now_ms);
    if (error == IOT_PROV_ERR_INVALID_STATE) {
        (void)send_json(response, 409,
                        error_body(error, IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (error == IOT_PROV_ERR_TEST_NOT_FOUND) {
        (void)send_json(response, 404,
                        error_body(error, IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (error != IOT_PROV_OK) {
        (void)send_json(response, 500,
                        error_body(error, IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    return build_current_status(adapter, request->now_ms, response, 200);
}

static bool path_matches(const char *path, const char *known)
{
    return strcmp(path, known) == 0;
}

iot_prov_error_t iot_http_adapter_init(
    iot_http_adapter_t *adapter,
    const char *ap_ssid,
    const char *firmware_version,
    iot_http_test_id_generator_t test_id_generator,
    void *generator_context,
    iot_http_commit_handler_t commit_handler,
    void *commit_context)
{
    iot_prov_error_t error;

    if (adapter == NULL || test_id_generator == NULL) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    memset(adapter, 0, sizeof(*adapter));
    error = iot_prov_session_init(&adapter->session, ap_ssid,
                                  firmware_version);
    if (error != IOT_PROV_OK) {
        return error;
    }
    adapter->initialized = true;
    adapter->test_id_generator = test_id_generator;
    adapter->generator_context = generator_context;
    adapter->commit_handler = commit_handler;
    adapter->commit_context = commit_context;
    return IOT_PROV_OK;
}

iot_prov_error_t iot_http_dispatch(
    iot_http_adapter_t *adapter,
    const iot_http_request_t *request,
    iot_http_response_t *response)
{
    if (adapter == NULL || !adapter->initialized || request == NULL ||
        response == NULL || request->method == NULL || request->path == NULL) {
        return IOT_PROV_ERR_INVALID_ARG;
    }

    response_clear(response);
    if (path_matches(request->path, STATUS_PATH)) {
        if (strcmp(request->method, "GET") == 0) {
            return handle_status(adapter, request, response);
        }
        (void)send_json(response, 405,
                        error_body(IOT_PROV_ERR_INVALID_STATE,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (path_matches(request->path, TEST_PATH)) {
        if (strcmp(request->method, "POST") == 0) {
            return handle_test(adapter, request, response);
        }
        (void)send_json(response, 405,
                        error_body(IOT_PROV_ERR_INVALID_STATE,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (path_matches(request->path, COMMIT_PATH)) {
        if (strcmp(request->method, "POST") == 0) {
            return handle_commit(adapter, request, response);
        }
        (void)send_json(response, 405,
                        error_body(IOT_PROV_ERR_INVALID_STATE,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }
    if (path_matches(request->path, CANCEL_PATH)) {
        if (strcmp(request->method, "POST") == 0) {
            return handle_cancel(adapter, request, response);
        }
        (void)send_json(response, 405,
                        error_body(IOT_PROV_ERR_INVALID_STATE,
                                   IOT_PROV_FIELD_NONE));
        return IOT_PROV_OK;
    }

    (void)send_json(response, 404,
                    error_body(IOT_PROV_ERR_INVALID_ARG,
                               IOT_PROV_FIELD_NONE));
    return IOT_PROV_OK;
}

iot_prov_error_t iot_http_set_commit_handler(
    iot_http_adapter_t *adapter,
    iot_http_commit_handler_t handler,
    void *context)
{
    if (adapter == NULL || !adapter->initialized) {
        return IOT_PROV_ERR_INVALID_ARG;
    }
    adapter->commit_handler = handler;
    adapter->commit_context = context;
    return IOT_PROV_OK;
}

iot_prov_session_t *iot_http_adapter_session(iot_http_adapter_t *adapter)
{
    if (adapter == NULL || !adapter->initialized) {
        return NULL;
    }
    return &adapter->session;
}
