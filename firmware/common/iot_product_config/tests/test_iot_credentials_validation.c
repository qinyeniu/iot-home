#include "iot_credentials_validation.h"
#include "iot_product_config_limits.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(condition) do {                                          \
    if (!(condition)) {                                                \
        printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__,     \
               __LINE__);                                              \
        ++s_failures;                                                  \
    }                                                                  \
} while (0)

static void fill(char *buffer, size_t size, char value)
{
    memset(buffer, value, size - 1U);
    buffer[size - 1U] = '\0';
}

static void test_wifi_valid_cases(void)
{
    iot_credentials_field_t field = IOT_CREDENTIALS_FIELD_NONE;

    CHECK(iot_credentials_validate_wifi("home", "secret", &field) ==
          IOT_CREDENTIALS_OK);
    CHECK(field == IOT_CREDENTIALS_FIELD_NONE);

    CHECK(iot_credentials_validate_wifi("home", "", &field) ==
          IOT_CREDENTIALS_OK);
    CHECK(field == IOT_CREDENTIALS_FIELD_NONE);

    CHECK(iot_credentials_validate_wifi("home-5g", "secret-123", &field) ==
          IOT_CREDENTIALS_OK);
    CHECK(field == IOT_CREDENTIALS_FIELD_NONE);
}

static void test_wifi_lengths(void)
{
    char ssid[IOT_WIFI_SSID_MAX_LEN + 1];
    char password[IOT_WIFI_PASSWORD_MAX_LEN + 1];
    iot_credentials_field_t field = IOT_CREDENTIALS_FIELD_NONE;

    fill(ssid, sizeof(ssid), 'a');
    fill(password, sizeof(password), 'b');
    CHECK(iot_credentials_validate_wifi(ssid, password, &field) ==
          IOT_CREDENTIALS_OK);
    CHECK(field == IOT_CREDENTIALS_FIELD_NONE);

    ssid[0] = 'x';
    CHECK(iot_credentials_validate_wifi(ssid, password, &field) ==
          IOT_CREDENTIALS_OK);

    /* Simulate a completely full, non-NUL-terminated fixed field. */
    memset(ssid, 'a', sizeof(ssid));
    CHECK(iot_credentials_validate_wifi(ssid, password, &field) ==
          IOT_CREDENTIALS_ERR_TOO_LONG);
    CHECK(field == IOT_CREDENTIALS_FIELD_WIFI_SSID);

    fill(ssid, sizeof(ssid), 'a');
    memset(password, 'b', sizeof(password));
    CHECK(iot_credentials_validate_wifi(ssid, password, &field) ==
          IOT_CREDENTIALS_ERR_TOO_LONG);
    CHECK(field == IOT_CREDENTIALS_FIELD_WIFI_PASSWORD);
}

static void test_wifi_required_and_control_chars(void)
{
    iot_credentials_field_t field = IOT_CREDENTIALS_FIELD_NONE;

    CHECK(iot_credentials_validate_wifi(NULL, "secret", &field) ==
          IOT_CREDENTIALS_ERR_NULL);
    CHECK(field == IOT_CREDENTIALS_FIELD_WIFI_SSID);

    CHECK(iot_credentials_validate_wifi("", "secret", &field) ==
          IOT_CREDENTIALS_ERR_EMPTY);
    CHECK(field == IOT_CREDENTIALS_FIELD_WIFI_SSID);

    CHECK(iot_credentials_validate_wifi("bad\tssid", "secret", &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);
    CHECK(field == IOT_CREDENTIALS_FIELD_WIFI_SSID);

    CHECK(iot_credentials_validate_wifi("home", "line\nbreak", &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);
    CHECK(field == IOT_CREDENTIALS_FIELD_WIFI_PASSWORD);

    CHECK(iot_credentials_validate_wifi("home", "del\x7f", &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);
    CHECK(field == IOT_CREDENTIALS_FIELD_WIFI_PASSWORD);

    CHECK(iot_credentials_validate_wifi("home", NULL, &field) ==
          IOT_CREDENTIALS_ERR_NULL);
    CHECK(field == IOT_CREDENTIALS_FIELD_WIFI_PASSWORD);

    CHECK(iot_credentials_validate_wifi("home", "secret", NULL) ==
          IOT_CREDENTIALS_OK);
}

static void test_mqtt_valid_cases(void)
{
    iot_credentials_field_t field = IOT_CREDENTIALS_FIELD_NONE;

    CHECK(iot_credentials_validate_mqtt(
              "mqtt://example.com:1883", "user", "pass", "gw-001",
              &field) == IOT_CREDENTIALS_OK);
    CHECK(field == IOT_CREDENTIALS_FIELD_NONE);

    CHECK(iot_credentials_validate_mqtt(
              "mqtt://example.com:1883", "", "", "", &field) ==
          IOT_CREDENTIALS_OK);
    CHECK(field == IOT_CREDENTIALS_FIELD_NONE);
}

static void test_mqtt_lengths(void)
{
    char uri[IOT_MQTT_URI_MAX_LEN + 1];
    char username[IOT_MQTT_USERNAME_MAX_LEN + 1];
    char password[IOT_MQTT_PASSWORD_MAX_LEN + 1];
    char client_id[IOT_MQTT_CLIENT_ID_MAX_LEN + 1];
    iot_credentials_field_t field = IOT_CREDENTIALS_FIELD_NONE;

    fill(uri, sizeof(uri), 'h');
    fill(username, sizeof(username), 'u');
    fill(password, sizeof(password), 'p');
    fill(client_id, sizeof(client_id), 'c');
    CHECK(iot_credentials_validate_mqtt(uri, username, password, client_id,
                                        &field) == IOT_CREDENTIALS_OK);
    CHECK(field == IOT_CREDENTIALS_FIELD_NONE);

    memset(uri, 'h', sizeof(uri));
    CHECK(iot_credentials_validate_mqtt(uri, username, password, client_id,
                                        &field) ==
          IOT_CREDENTIALS_ERR_TOO_LONG);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_URI);

    fill(uri, sizeof(uri), 'h');
    memset(username, 'u', sizeof(username));
    CHECK(iot_credentials_validate_mqtt(uri, username, password, client_id,
                                        &field) ==
          IOT_CREDENTIALS_ERR_TOO_LONG);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_USERNAME);

    fill(username, sizeof(username), 'u');
    memset(password, 'p', sizeof(password));
    CHECK(iot_credentials_validate_mqtt(uri, username, password, client_id,
                                        &field) ==
          IOT_CREDENTIALS_ERR_TOO_LONG);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_PASSWORD);

    fill(password, sizeof(password), 'p');
    memset(client_id, 'c', sizeof(client_id));
    CHECK(iot_credentials_validate_mqtt(uri, username, password, client_id,
                                        &field) ==
          IOT_CREDENTIALS_ERR_TOO_LONG);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_CLIENT_ID);
}

static void test_mqtt_required_and_control_chars(void)
{
    iot_credentials_field_t field = IOT_CREDENTIALS_FIELD_NONE;

    CHECK(iot_credentials_validate_mqtt(NULL, "", "", "", &field) ==
          IOT_CREDENTIALS_ERR_NULL);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_URI);

    CHECK(iot_credentials_validate_mqtt("", "", "", "", &field) ==
          IOT_CREDENTIALS_ERR_EMPTY);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_URI);

    CHECK(iot_credentials_validate_mqtt("mqtt://bad", "us\rer", "", "",
                                        &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_USERNAME);

    CHECK(iot_credentials_validate_mqtt("mqtt://example", "", "pa\tss", "",
                                        &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_PASSWORD);

    CHECK(iot_credentials_validate_mqtt("mqtt://example", "", "", "cid\x01",
                                        &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_CLIENT_ID);

    CHECK(iot_credentials_validate_mqtt("mqtt://example", NULL, "", "",
                                        &field) ==
          IOT_CREDENTIALS_ERR_NULL);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_USERNAME);

    CHECK(iot_credentials_validate_mqtt("mqtt://example", "", NULL, "",
                                        &field) ==
          IOT_CREDENTIALS_ERR_NULL);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_PASSWORD);

    CHECK(iot_credentials_validate_mqtt("mqtt://example", "", "", NULL,
                                        &field) ==
          IOT_CREDENTIALS_ERR_NULL);
    CHECK(field == IOT_CREDENTIALS_FIELD_MQTT_CLIENT_ID);
}

static void test_text_policy_and_capacity(void)
{
    iot_credentials_field_t field = IOT_CREDENTIALS_FIELD_NONE;

    /* Wi-Fi is an octet string: high bytes are allowed, ASCII controls are not. */
    CHECK(iot_credentials_validate_text("raw\x80ssid", 33U, 32U, true, false,
                                        IOT_CREDENTIALS_FIELD_WIFI_SSID,
                                        &field) == IOT_CREDENTIALS_OK);
    CHECK(iot_credentials_validate_text("raw\x9fssid", 33U, 32U, true, false,
                                        IOT_CREDENTIALS_FIELD_WIFI_SSID,
                                        &field) == IOT_CREDENTIALS_OK);
    CHECK(iot_credentials_validate_text("raw\x01ssid", 33U, 32U, true, false,
                                        IOT_CREDENTIALS_FIELD_WIFI_SSID,
                                        &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);

    /* MQTT fields require valid UTF-8 and reject raw or encoded C1 controls. */
    CHECK(iot_credentials_validate_text("mqtt://raw\x80", 161U, 160U, true,
                                        true, IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("mqtt://raw\xc2\x85", 161U, 160U,
                                        true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);
    CHECK(iot_credentials_validate_text("mqtt://raw\xc2\x9f", 161U, 160U,
                                        true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_CONTROL_CHAR);
    CHECK(iot_credentials_validate_text("mqtt://home", 12U, 11U, true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) == IOT_CREDENTIALS_OK);
    CHECK(iot_credentials_validate_text("mqtt://home", 9U, 9U, true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_TOO_LONG);
    CHECK(iot_credentials_validate_text("\xed\xa0\x80", 65U, 64U, false, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_USERNAME,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xc0\x80", 161U, 160U, true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xc1\xbf", 161U, 160U, true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xe0\x80\x80", 161U, 160U, true,
                                        true, IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xf0\x80\x80\x80", 161U, 160U, true,
                                        true, IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xc2\xff", 161U, 160U, true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xc2\x41", 161U, 160U, true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xe0\xa0\x2e", 161U, 160U, true,
                                        true, IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xc2", 161U, 160U, true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xe0\xa0", 161U, 160U, true, true,
                                        IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
    CHECK(iot_credentials_validate_text("\xf0\x90\x80", 161U, 160U, true,
                                        true, IOT_CREDENTIALS_FIELD_MQTT_URI,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);    CHECK(iot_credentials_validate_text("\xf4\x90\x80\x80", 65U, 64U, false,
                                        true,
                                        IOT_CREDENTIALS_FIELD_MQTT_USERNAME,
                                        &field) ==
          IOT_CREDENTIALS_ERR_INVALID_ENCODING);
}
int main(void)
{
    test_wifi_valid_cases();
    test_wifi_lengths();
    test_wifi_required_and_control_chars();
    test_mqtt_valid_cases();
    test_mqtt_lengths();
    test_mqtt_required_and_control_chars();
    test_text_policy_and_capacity();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all credential validation checks passed\n");
    return 0;
}

