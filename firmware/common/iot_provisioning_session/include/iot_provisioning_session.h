/*
 * IoT-Home SoftAP provisioning session core.
 *
 * Host-compatible state machine for validating and retaining one RAM-only
 * provisioning test. It does not start Wi-Fi/SoftAP, open a socket or write
 * NVS. The integration layer is responsible for those actions.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_PROV_AP_SSID_MAX_LEN          32
#define IOT_PROV_FIRMWARE_VERSION_MAX_LEN 32
#define IOT_PROV_TEST_ID_MAX_LEN          32
#define IOT_PROV_MQTT_HOST_MAX_LEN        253
#define IOT_PROV_ERROR_CAPACITY           4

typedef enum {
    IOT_PROV_STATE_WAITING = 0,
    IOT_PROV_STATE_TESTING,
    IOT_PROV_STATE_TEST_PASSED,
    IOT_PROV_STATE_TEST_FAILED,
    IOT_PROV_STATE_COMMITTED,
    IOT_PROV_STATE_ERROR,
} iot_prov_state_t;

typedef enum {
    IOT_PROV_STEP_IDLE = 0,
    IOT_PROV_STEP_PENDING,
    IOT_PROV_STEP_PASSED,
    IOT_PROV_STEP_FAILED,
} iot_prov_step_t;

typedef enum {
    IOT_PROV_OK = 0,
    IOT_PROV_ERR_INVALID_ARG,
    IOT_PROV_ERR_INVALID_STATE,
    IOT_PROV_ERR_INVALID_FIELD,
    IOT_PROV_ERR_WIFI_AUTH_FAILED,
    IOT_PROV_ERR_WIFI_ASSOC_TIMEOUT,
    IOT_PROV_ERR_WIFI_NO_IP_ADDRESS,
    IOT_PROV_ERR_DNS_FAILED,
    IOT_PROV_ERR_MQTT_CONNECT_FAILED,
    IOT_PROV_ERR_TLS_NOT_CONFIGURED,
    IOT_PROV_ERR_RATE_LIMITED,
    IOT_PROV_ERR_TEST_ACTIVE,
    IOT_PROV_ERR_TEST_NOT_FOUND,
    IOT_PROV_ERR_TEST_EXPIRED,
    IOT_PROV_ERR_TEST_NOT_PASSED,
    IOT_PROV_ERR_INTERNAL,
} iot_prov_error_t;

typedef enum {
    IOT_PROV_FIELD_NONE = 0,
    IOT_PROV_FIELD_WIFI_SSID,
    IOT_PROV_FIELD_WIFI_PASSWORD,
    IOT_PROV_FIELD_OPEN_NETWORK,
    IOT_PROV_FIELD_MQTT_HOST,
    IOT_PROV_FIELD_MQTT_PORT,
    IOT_PROV_FIELD_MQTT_TLS,
    IOT_PROV_FIELD_MQTT_USERNAME,
    IOT_PROV_FIELD_MQTT_PASSWORD,
    IOT_PROV_FIELD_GATEWAY_ID,
} iot_prov_field_t;

typedef struct {
    char wifi_ssid[IOT_PROV_AP_SSID_MAX_LEN + 1];
    char wifi_password[64];
    bool open_network;
    char mqtt_host[IOT_PROV_MQTT_HOST_MAX_LEN + 1];
    uint16_t mqtt_port;
    bool mqtt_tls;
    char mqtt_username[65];
    char mqtt_password[65];
    char gateway_id[65];
} iot_prov_request_t;

typedef struct {
    char wifi_ssid[IOT_PROV_AP_SSID_MAX_LEN + 1];
    char wifi_password[64];
    char mqtt_uri[161];
    char mqtt_username[65];
    char mqtt_password[65];
    char mqtt_client_id[65];
} iot_prov_bundle_t;

typedef struct {
    iot_prov_error_t code;
    iot_prov_field_t field;
} iot_prov_error_info_t;

typedef struct {
    iot_prov_state_t state;
    bool can_commit;
    uint32_t started_at_ms;
    uint32_t expires_at_ms;
    iot_prov_step_t input;
    iot_prov_step_t wifi;
    iot_prov_step_t mqtt;
    uint8_t error_count;
    iot_prov_error_info_t errors[IOT_PROV_ERROR_CAPACITY];
} iot_prov_status_t;

/*
 * RAM-only provisioning session. The object is not internally synchronized:
 * all calls must come from one task, or the integration layer must hold a lock.
 */
typedef struct iot_prov_session {
    bool initialized;
    char ap_ssid[IOT_PROV_AP_SSID_MAX_LEN + 1];
    char firmware_version[IOT_PROV_FIRMWARE_VERSION_MAX_LEN + 1];

    iot_prov_state_t state;
    iot_prov_step_t input;
    iot_prov_step_t wifi;
    iot_prov_step_t mqtt;

    bool test_active;
    char test_id[IOT_PROV_TEST_ID_MAX_LEN + 1];
    uint32_t started_at_ms;
    iot_prov_bundle_t snapshot;

    uint8_t error_count;
    iot_prov_error_info_t errors[IOT_PROV_ERROR_CAPACITY];

    uint32_t request_times_ms[5];
    uint8_t request_count;
    uint8_t request_ring_index;
} iot_prov_session_t;

/**
 * Initialize a provisioning session.
 *
 * @param ap_ssid SoftAP SSID displayed in status; not a credential.
 * @param firmware_version Short firmware version string.
 */
iot_prov_error_t iot_prov_session_init(
    iot_prov_session_t *session,
    const char *ap_ssid,
    const char *firmware_version);

/**
 * Validate and start one RAM-only provisioning test.
 *
 * @param test_id Unique caller-generated identifier shaped like
 *                tmp-<alphanumeric>; it must never be reused in this session.
 * @param now_ms Monotonic millisecond timestamp.
 */
iot_prov_error_t iot_prov_begin(
    iot_prov_session_t *session,
    const iot_prov_request_t *request,
    const char *test_id,
    uint32_t now_ms);

/** Report the Wi-Fi test result for the active test. */
iot_prov_error_t iot_prov_note_wifi_result(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms,
    bool passed,
    iot_prov_error_t error_code,
    iot_prov_field_t error_field);

/** Report the MQTT test result for the active test. */
iot_prov_error_t iot_prov_note_mqtt_result(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms,
    bool passed,
    iot_prov_error_t error_code,
    iot_prov_field_t error_field);

/**
 * Return the immutable snapshot only if its test passed and has not expired.
 * This function does not write NVS.
 */
iot_prov_error_t iot_prov_prepare_commit(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms,
    iot_prov_bundle_t *bundle);

/** Mark the session committed after the caller successfully saved the bundle. */
iot_prov_error_t iot_prov_commit_succeeded(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms);

/** Mark a commit/save failure without pretending the bundle was stored. */
iot_prov_error_t iot_prov_commit_failed(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms,
    iot_prov_error_t error_code,
    iot_prov_field_t error_field);

/** Cancel the active or failed test. NULL test_id cancels any active test. */
iot_prov_error_t iot_prov_cancel(
    iot_prov_session_t *session,
    const char *test_id,
    uint32_t now_ms);

/** Read the current status after applying time-based expiration. */
iot_prov_error_t iot_prov_get_status(
    iot_prov_session_t *session,
    uint32_t now_ms,
    iot_prov_status_t *status);

#ifdef __cplusplus
}
#endif


