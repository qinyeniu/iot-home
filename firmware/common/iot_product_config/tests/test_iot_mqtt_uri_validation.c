#include "iot_mqtt_uri_validation.h"
#include "iot_product_config_limits.h"

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

static iot_mqtt_uri_error_t validate(const char *uri)
{
    return iot_mqtt_uri_validate(uri, strlen(uri) + 1U);
}

static void test_valid_uris(void)
{
    CHECK(validate("mqtt://8.163.110.27:1883") == IOT_MQTT_URI_OK);
    CHECK(validate("mqtt://8.163.110.27") == IOT_MQTT_URI_OK);
    CHECK(validate("mqtt://example.com") == IOT_MQTT_URI_OK);
    CHECK(validate("mqtt://broker.example.com:1883") == IOT_MQTT_URI_OK);
    CHECK(validate("mqtt://localhost") == IOT_MQTT_URI_OK);
    CHECK(validate("mqtts://example.com:8883") == IOT_MQTT_URI_OK);
    CHECK(validate("ws://example.com/mqtt") == IOT_MQTT_URI_OK);
    CHECK(validate("ws://example.com:8080/mqtt?protocol=3.1.1") ==
          IOT_MQTT_URI_OK);
    CHECK(validate("wss://example.com:443/mqtt") == IOT_MQTT_URI_OK);

    /* Scheme names are case-insensitive per URI syntax. */
    CHECK(validate("MQTT://example.com") == IOT_MQTT_URI_OK);
    CHECK(validate("MqttS://example.com:8883") == IOT_MQTT_URI_OK);
    CHECK(validate("WSS://example.com/mqtt") == IOT_MQTT_URI_OK);
    CHECK(validate("Ws://example.com:8080/mqtt") == IOT_MQTT_URI_OK);
}

static void test_scheme_errors(void)
{
    CHECK(validate("tcp://example.com") == IOT_MQTT_URI_ERR_UNSUPPORTED_SCHEME);
    CHECK(validate("http://example.com") == IOT_MQTT_URI_ERR_UNSUPPORTED_SCHEME);
    CHECK(validate("mqtt:/example.com") == IOT_MQTT_URI_ERR_UNSUPPORTED_SCHEME);
    CHECK(validate("mqtt://") == IOT_MQTT_URI_ERR_HOST);
    CHECK(iot_mqtt_uri_validate(NULL, 10U) == IOT_MQTT_URI_ERR_NULL);
    CHECK(iot_mqtt_uri_validate("mqtt://example.com", 0U) ==
          IOT_MQTT_URI_ERR_TOO_LONG);
    CHECK(iot_mqtt_uri_validate("mqtt://example.com",
                                IOT_MQTT_URI_MAX_LEN + 2U) ==
          IOT_MQTT_URI_ERR_TOO_LONG);
}

static void test_capacity_boundaries(void)
{
    char uri[IOT_MQTT_URI_MAX_LEN + 1];
    char bounded[64];
    const char prefix[] = "ws://example.com/";
    size_t prefix_len = strlen(prefix);

    memset(uri, 'a', sizeof(uri));
    memcpy(uri, prefix, prefix_len);
    uri[IOT_MQTT_URI_MAX_LEN] = '\0';

    CHECK(iot_mqtt_uri_validate(uri, sizeof(uri)) == IOT_MQTT_URI_OK);

    /* The same buffer is not NUL-terminated within this smaller capacity. */
    CHECK(iot_mqtt_uri_validate(uri, IOT_MQTT_URI_MAX_LEN) ==
          IOT_MQTT_URI_ERR_TOO_LONG);

    /* A smaller capacity still includes the NUL and bounds parsing safely. */
    memcpy(bounded, "mqtt://example.com", strlen("mqtt://example.com") + 1U);
    CHECK(iot_mqtt_uri_validate(bounded, strlen("mqtt://example.com") + 1U) ==
          IOT_MQTT_URI_OK);

    memcpy(bounded, "ws://example.com/mqtt",
           strlen("ws://example.com/mqtt") + 1U);
    bounded[prefix_len - 1U] = '\0';
    CHECK(iot_mqtt_uri_validate(bounded, sizeof(bounded)) ==
          IOT_MQTT_URI_ERR_PATH);
}

static void test_host_errors(void)
{
    CHECK(validate("mqtt://999.1.1.1") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://1.2.3") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://1.2.3.4.5") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://1.2.3.4.") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://01.2.3.4") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://example..com") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://.example.com") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://example.com.") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://[2001:db8::1]") == IOT_MQTT_URI_ERR_HOST);
    CHECK(validate("mqtt://exa mple.com") == IOT_MQTT_URI_ERR_HOST);
}

static void test_port_errors(void)
{
    CHECK(validate("mqtt://example.com:") == IOT_MQTT_URI_ERR_PORT);
    CHECK(validate("mqtt://example.com:0") == IOT_MQTT_URI_ERR_PORT);
    CHECK(validate("mqtt://example.com:65536") == IOT_MQTT_URI_ERR_PORT);
    CHECK(validate("mqtt://example.com:abc") == IOT_MQTT_URI_ERR_PORT);
    CHECK(validate("mqtt://example.com:80:81") == IOT_MQTT_URI_ERR_HOST);
}

static void test_path_and_userinfo_errors(void)
{
    CHECK(validate("mqtt://user:pass@example.com") ==
          IOT_MQTT_URI_ERR_USERINFO);
    CHECK(validate("mqtt://example.com/") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("mqtt://example.com/mqtt") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("mqtt://example.com?x=1") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com#frag") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com/%zz") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com/%") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com/%4") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com/%4G") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com/mqtt\t") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com/mqtt?q=a\nb") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com/mqtt?q=a\rb") == IOT_MQTT_URI_ERR_PATH);
    CHECK(validate("ws://example.com/a%41/b?x=%4") == IOT_MQTT_URI_ERR_PATH);

    CHECK(validate("ws://example.com/a%41") == IOT_MQTT_URI_OK);
}

int main(void)
{
    test_valid_uris();
    test_scheme_errors();
    test_capacity_boundaries();
    test_host_errors();
    test_port_errors();
    test_path_and_userinfo_errors();

    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("all MQTT URI validation checks passed\n");
    return 0;
}
