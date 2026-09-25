#include "iot_provisioning_http.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static int s_failures;

#define CHECK(condition) do {                                         \
    if (!(condition)) {                                               \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,    \
               __LINE__);                                             \
        ++s_failures;                                                 \
    }                                                                 \
} while (0)

static bool test_id_generator(char *test_id, size_t capacity, void *context)
{
    uint32_t *counter = context;
    int written;

    ++(*counter);
    written = snprintf(test_id, capacity, "tmp-test%04lu",
                       (unsigned long)*counter);
    return written > 0 && (size_t)written < capacity;
}

static iot_http_commit_result_t commit_success(
    const iot_prov_bundle_t *bundle, void *context)
{
    iot_http_commit_result_t result = {
        .code = IOT_PROV_OK,
        .field = IOT_PROV_FIELD_NONE,
    };

    (void)bundle;
    (void)context;
    return result;
}

static iot_http_commit_result_t commit_fail(
    const iot_prov_bundle_t *bundle, void *context)
{
    iot_http_commit_result_t result = {
        .code = IOT_PROV_ERR_INTERNAL,
        .field = IOT_PROV_FIELD_NONE,
    };

    (void)bundle;
    (void)context;
    return result;
}

static iot_http_request_t make_request(
    const char *method, const char *path, const char *content_type,
    const char *body, uint32_t now_ms)
{
    iot_http_request_t request;

    memset(&request, 0, sizeof(request));
    request.method = method;
    request.path = path;
    request.content_type = content_type;
    request.body = body;
    request.now_ms = now_ms;
    if (body != NULL) {
        request.body_len = strlen(body);
    }
    return request;
}

static cJSON *response_json(const iot_http_response_t *response)
{
    cJSON *root = cJSON_Parse(response->body);

    CHECK(root != NULL);
    return root;
}

static void init_adapter(iot_http_adapter_t *adapter, uint32_t *counter,
                         iot_http_commit_handler_t handler)
{
    CHECK(iot_http_adapter_init(
              adapter, "IoT-Home-3F9A", "0.1.0",
              test_id_generator, counter, handler, NULL) == IOT_PROV_OK);
}

static const char *valid_body(void)
{
    return "{"
           "\"wifi_ssid\":\"HomeWiFi\","
           "\"wifi_password\":\"password123\","
           "\"open_network\":false,"
           "\"mqtt_host\":\"mqtt.example.com\","
           "\"mqtt_port\":1883,"
           "\"mqtt_tls\":false,"
           "\"mqtt_username\":\"gateway\","
           "\"mqtt_password\":\"mqtt-secret\","
           "\"gateway_id\":\"gw-001\""
           "}";
}

static void start_passing_http_test(iot_http_adapter_t *adapter,
                                    uint32_t now_ms,
                                    char *test_id,
                                    size_t test_id_size)
{
    iot_http_request_t request = make_request(
        "POST", "/api/provision/test", "application/json", valid_body(),
        now_ms);
    iot_http_response_t response;
    cJSON *root;

    CHECK(iot_http_dispatch(adapter, &request, &response) == IOT_PROV_OK);
    CHECK(response.status_code == 202);
    root = response_json(&response);
    CHECK(cJSON_GetObjectItem(root, "state") != NULL);
    CHECK(strcmp(cJSON_GetObjectItem(root, "state")->valuestring,
                 "testing") == 0);
    snprintf(test_id, test_id_size, "%s",
             cJSON_GetObjectItem(root, "test_id")->valuestring);
    cJSON_Delete(root);
}

static void mark_test_passed(iot_http_adapter_t *adapter,
                             const char *test_id, uint32_t now_ms)
{
    iot_prov_session_t *session = iot_http_adapter_session(adapter);

    CHECK(iot_prov_note_wifi_result(session, test_id, now_ms + 1, true,
                                    IOT_PROV_OK,
                                    IOT_PROV_FIELD_NONE) == IOT_PROV_OK);
    CHECK(iot_prov_note_mqtt_result(session, test_id, now_ms + 2, true,
                                    IOT_PROV_OK,
                                    IOT_PROV_FIELD_NONE) == IOT_PROV_OK);
}

static void test_initial_status(void)
{
    iot_http_adapter_t adapter;
    iot_http_request_t request = make_request(
        "GET", "/api/provision/status", NULL, NULL, 100U);
    iot_http_response_t response;
    cJSON *root;
    cJSON *tests;
    uint32_t counter = 0;

    init_adapter(&adapter, &counter, commit_success);
    CHECK(iot_http_dispatch(&adapter, &request, &response) == IOT_PROV_OK);
    CHECK(response.status_code == 200);
    root = response_json(&response);
    CHECK(strcmp(cJSON_GetObjectItem(root, "state")->valuestring,
                 "waiting") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(root, "ap_ssid")->valuestring,
                 "IoT-Home-3F9A") == 0);
    CHECK(cJSON_GetObjectItem(root, "can_commit")->valueint == 0);
    CHECK(cJSON_GetObjectItem(root, "test_expires_at")->type == cJSON_NULL);
    tests = cJSON_GetObjectItem(root, "tests");
    CHECK(strcmp(cJSON_GetObjectItem(tests, "input")->valuestring,
                 "idle") == 0);
    cJSON_Delete(root);
}

static void test_full_success(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    cJSON *root;
    char test_id[33];
    uint32_t counter = 0;
    iot_http_request_t status_request = make_request(
        "GET", "/api/provision/status", NULL, NULL, 1003U);
    iot_http_request_t commit_request;

    init_adapter(&adapter, &counter, commit_success);
    start_passing_http_test(&adapter, 1000U, test_id, sizeof(test_id));
    mark_test_passed(&adapter, test_id, 1000U);

    CHECK(iot_http_dispatch(&adapter, &status_request, &response) ==
          IOT_PROV_OK);
    root = response_json(&response);
    CHECK(strcmp(cJSON_GetObjectItem(root, "state")->valuestring,
                 "test_passed") == 0);
    CHECK(cJSON_GetObjectItem(root, "can_commit")->valueint == 1);
    CHECK(strstr(response.body, "password123") == NULL);
    CHECK(strstr(response.body, "mqtt-secret") == NULL);
    cJSON_Delete(root);

    commit_request = make_request(
        "POST", "/api/provision/commit", "application/json",
        "{\"test_id\":\"tmp-test0001\"}", 1004U);
    CHECK(iot_http_dispatch(&adapter, &commit_request, &response) ==
          IOT_PROV_OK);
    CHECK(response.status_code == 200);
    root = response_json(&response);
    CHECK(strcmp(cJSON_GetObjectItem(root, "state")->valuestring,
                 "committed") == 0);
    cJSON_Delete(root);
}

static void test_commit_failure(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    cJSON *root;
    char test_id[33];
    uint32_t counter = 0;
    iot_http_request_t commit_request = make_request(
        "POST", "/api/provision/commit", "application/json",
        "{\"test_id\":\"tmp-test0001\"}", 2002U);
    iot_http_request_t second_commit = make_request(
        "POST", "/api/provision/commit", "application/json",
        "{\"test_id\":\"tmp-test0001\"}", 2003U);

    init_adapter(&adapter, &counter, commit_fail);
    start_passing_http_test(&adapter, 2000U, test_id, sizeof(test_id));
    mark_test_passed(&adapter, test_id, 2000U);

    CHECK(iot_http_dispatch(&adapter, &commit_request, &response) ==
          IOT_PROV_OK);
    CHECK(response.status_code == 500);
    root = response_json(&response);
    CHECK(strcmp(cJSON_GetObjectItem(root, "state")->valuestring,
                 "error") == 0);
    cJSON_Delete(root);

    CHECK(iot_http_dispatch(&adapter, &second_commit, &response) ==
          IOT_PROV_OK);
    CHECK(response.status_code == 409);
}

static void test_missing_commit_handler(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    char test_id[33];
    uint32_t counter = 0;
    iot_http_request_t commit_request = make_request(
        "POST", "/api/provision/commit", "application/json",
        "{\"test_id\":\"tmp-test0001\"}", 3002U);

    init_adapter(&adapter, &counter, NULL);
    start_passing_http_test(&adapter, 3000U, test_id, sizeof(test_id));
    mark_test_passed(&adapter, test_id, 3000U);
    CHECK(iot_http_dispatch(&adapter, &commit_request, &response) ==
          IOT_PROV_OK);
    CHECK(response.status_code == 501);
}

static void test_routing_errors(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    iot_http_request_t unknown = make_request(
        "GET", "/nowhere", NULL, NULL, 100U);
    iot_http_request_t wrong_method = make_request(
        "POST", "/api/provision/status", "application/json", "{}", 101U);
    uint32_t counter = 0;

    init_adapter(&adapter, &counter, commit_success);
    CHECK(iot_http_dispatch(&adapter, &unknown, &response) == IOT_PROV_OK);
    CHECK(response.status_code == 404);
    CHECK(iot_http_dispatch(&adapter, &wrong_method, &response) ==
          IOT_PROV_OK);
    CHECK(response.status_code == 405);
}

static void test_request_errors(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    uint32_t counter = 0;

    init_adapter(&adapter, &counter, commit_success);

    {
        iot_http_request_t bad_json = make_request(
            "POST", "/api/provision/test", "application/json", "{bad", 100U);
        CHECK(iot_http_dispatch(&adapter, &bad_json, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 400);
    }
    {
        iot_http_request_t wrong_type = make_request(
            "POST", "/api/provision/test", "text/plain", "{}", 101U);
        CHECK(iot_http_dispatch(&adapter, &wrong_type, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 415);
    }
    {
        iot_http_request_t missing_field = make_request(
            "POST", "/api/provision/test", "application/json", "{}", 102U);
        cJSON *root;
        cJSON *errors;

        CHECK(iot_http_dispatch(&adapter, &missing_field, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 400);
        root = response_json(&response);
        errors = cJSON_GetObjectItem(root, "errors");
        CHECK(cJSON_GetArraySize(errors) == 1);
        CHECK(strcmp(cJSON_GetArrayItem(errors, 0)->child->valuestring,
                     "INVALID_FIELD") == 0);
        cJSON_Delete(root);
    }
    {
        const char *tls_body =
            "{\"wifi_ssid\":\"HomeWiFi\",\"wifi_password\":\"password123\","
            "\"mqtt_host\":\"mqtt.example.com\",\"mqtt_port\":1883,"
            "\"mqtt_tls\":true}";
        iot_http_request_t tls_request = make_request(
            "POST", "/api/provision/test", "application/json", tls_body,
            103U);
        cJSON *root;
        cJSON *errors;

        CHECK(iot_http_dispatch(&adapter, &tls_request, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 400);
        root = response_json(&response);
        errors = cJSON_GetObjectItem(root, "errors");
        CHECK(strcmp(cJSON_GetArrayItem(errors, 0)->child->valuestring,
                     "TLS_NOT_CONFIGURED") == 0);
        cJSON_Delete(root);
    }
}

static void test_body_too_large(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    iot_http_request_t request;
    char *large;
    uint32_t counter = 0;

    init_adapter(&adapter, &counter, commit_success);
    large = malloc(IOT_HTTP_MAX_BODY + 2U);
    if (large == NULL) {
        CHECK(large != NULL);
        return;
    }
    memset(large, 'a', IOT_HTTP_MAX_BODY + 1U);
    large[IOT_HTTP_MAX_BODY + 1U] = '\0';

    memset(&request, 0, sizeof(request));
    request.method = "POST";
    request.path = "/api/provision/test";
    request.content_type = "application/json";
    request.body = large;
    request.body_len = IOT_HTTP_MAX_BODY + 1U;
    request.now_ms = 100U;

    CHECK(iot_http_dispatch(&adapter, &request, &response) == IOT_PROV_OK);
    CHECK(response.status_code == 413);
    free(large);
}

static void test_active_and_rate_limit(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    uint32_t counter = 0;

    init_adapter(&adapter, &counter, commit_success);

    for (uint8_t i = 0U; i < 5U; ++i) {
        iot_http_request_t request = make_request(
            "POST", "/api/provision/test", "application/json", valid_body(),
            100U + (uint32_t)i);

        CHECK(iot_http_dispatch(&adapter, &request, &response) ==
              IOT_PROV_OK);
        if (i == 0U) {
            CHECK(response.status_code == 202);
        } else {
            CHECK(response.status_code == 409);
        }
    }

    {
        iot_http_request_t request = make_request(
            "POST", "/api/provision/test", "application/json", valid_body(),
            105U);
        CHECK(iot_http_dispatch(&adapter, &request, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 429);
    }
}

static void test_invalid_test_requests_rate_limited(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    uint32_t counter = 0;

    init_adapter(&adapter, &counter, commit_success);

    {
        iot_http_request_t malformed = make_request(
            "POST", "/api/provision/test", "application/json", "{bad", 100U);
        CHECK(iot_http_dispatch(&adapter, &malformed, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 400);
    }
    {
        iot_http_request_t wrong_type = make_request(
            "POST", "/api/provision/test", "text/plain", "{}", 101U);
        CHECK(iot_http_dispatch(&adapter, &wrong_type, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 415);
    }
    {
        iot_http_request_t too_large;

        memset(&too_large, 0, sizeof(too_large));
        too_large.method = "POST";
        too_large.path = "/api/provision/test";
        too_large.content_type = "application/json";
        too_large.body = "dummy";
        too_large.body_len = IOT_HTTP_MAX_BODY + 1U;
        too_large.now_ms = 102U;
        CHECK(iot_http_dispatch(&adapter, &too_large, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 413);
    }
    {
        iot_http_request_t empty = make_request(
            "POST", "/api/provision/test", NULL, NULL, 103U);
        CHECK(iot_http_dispatch(&adapter, &empty, &response) == IOT_PROV_OK);
        CHECK(response.status_code == 400);
    }
    {
        iot_http_request_t missing_field = make_request(
            "POST", "/api/provision/test", "application/json", "{}", 104U);
        CHECK(iot_http_dispatch(&adapter, &missing_field, &response) ==
              IOT_PROV_OK);
        CHECK(response.status_code == 400);
    }
    {
        iot_http_request_t valid = make_request(
            "POST", "/api/provision/test", "application/json", valid_body(),
            105U);
        CHECK(iot_http_dispatch(&adapter, &valid, &response) == IOT_PROV_OK);
        CHECK(response.status_code == 429);
        CHECK(strstr(response.body, "password123") == NULL);
        CHECK(strstr(response.body, "mqtt-secret") == NULL);
    }
}

static void test_cancel(void)
{
    iot_http_adapter_t adapter;
    iot_http_response_t response;
    char test_id[33];
    uint32_t counter = 0;
    iot_http_request_t cancel_request = make_request(
        "POST", "/api/provision/cancel", "application/json",
        "{\"test_id\":\"tmp-test0001\"}", 1002U);

    iot_http_request_t bad_cancel_request = make_request(
        "POST", "/api/provision/cancel", "application/json",
        "{\"test_id\":123}", 1001U);
    iot_http_request_t status_request = make_request(
        "GET", "/api/provision/status", NULL, NULL, 1001U);

    init_adapter(&adapter, &counter, commit_success);
    start_passing_http_test(&adapter, 1000U, test_id, sizeof(test_id));
    CHECK(iot_http_dispatch(&adapter, &bad_cancel_request, &response) ==
          IOT_PROV_OK);
    CHECK(response.status_code == 400);
    CHECK(iot_http_dispatch(&adapter, &status_request, &response) ==
          IOT_PROV_OK);
    {
        cJSON *root = response_json(&response);
        CHECK(strcmp(cJSON_GetObjectItem(root, "state")->valuestring,
                     "testing") == 0);
        cJSON_Delete(root);
    }
    CHECK(iot_http_dispatch(&adapter, &cancel_request, &response) ==
          IOT_PROV_OK);
    CHECK(response.status_code == 200);
    {
        cJSON *root = response_json(&response);
        CHECK(strcmp(cJSON_GetObjectItem(root, "state")->valuestring,
                     "waiting") == 0);
        cJSON_Delete(root);
    }
}

int main(void)
{
    test_initial_status();
    test_full_success();
    test_commit_failure();
    test_missing_commit_handler();
    test_routing_errors();
    test_request_errors();
    test_body_too_large();
    test_active_and_rate_limit();
    test_invalid_test_requests_rate_limited();
    test_cancel();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all provisioning HTTP adapter checks passed\n");
    return 0;
}
