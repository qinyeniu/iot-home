/*
 * Host-compatible MQTT URI validation.
 */

#include "iot_mqtt_uri_validation.h"
#include "iot_product_config_limits.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    URI_SCHEME_MQTT = 0,
    URI_SCHEME_MQTTS,
    URI_SCHEME_WS,
    URI_SCHEME_WSS,
} uri_scheme_t;

static bool char_is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static bool char_is_alpha(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static bool char_is_unreserved(char ch)
{
    return char_is_alpha(ch) || char_is_digit(ch) ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}
static bool char_is_hex_safe(char ch)
{
    return char_is_digit(ch) ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static char lower_ascii(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
}

static bool parse_scheme(const char *uri, size_t scheme_end,
                         uri_scheme_t *scheme)
{
    if (scheme_end == 4U &&
        lower_ascii(uri[0]) == 'm' && lower_ascii(uri[1]) == 'q' &&
        lower_ascii(uri[2]) == 't' && lower_ascii(uri[3]) == 't') {
        *scheme = URI_SCHEME_MQTT;
        return true;
    }
    if (scheme_end == 2U &&
        lower_ascii(uri[0]) == 'w' && lower_ascii(uri[1]) == 's') {
        *scheme = URI_SCHEME_WS;
        return true;
    }
    if (scheme_end == 5U &&
        lower_ascii(uri[0]) == 'm' && lower_ascii(uri[1]) == 'q' &&
        lower_ascii(uri[2]) == 't' && lower_ascii(uri[3]) == 't' &&
        lower_ascii(uri[4]) == 's') {
        *scheme = URI_SCHEME_MQTTS;
        return true;
    }
    if (scheme_end == 3U &&
        lower_ascii(uri[0]) == 'w' && lower_ascii(uri[1]) == 's' &&
        lower_ascii(uri[2]) == 's') {
        *scheme = URI_SCHEME_WSS;
        return true;
    }
    return false;
}

static bool parse_ipv4(const char *host, size_t len)
{
    int octet = -1;
    int dots = 0;

    if (len == 0U || host[len - 1U] == '.') {
        return false;
    }

    for (size_t i = 0; i <= len; ++i) {
        char ch = i == len ? '.' : host[i];

        if (ch == '.') {
            if (octet < 0 || octet > 255) {
                return false;
            }
            octet = -1;
            if (i != len) {
                ++dots;
            }
        } else if (!char_is_digit(ch)) {
            return false;
        } else {
            if (octet < 0) {
                octet = 0;
            }
            /* Reject leading zero, but a single zero octet is valid. */
            if (octet == 0 && ch == '0' && i + 1U < len &&
                host[i + 1U] != '.') {
                return false;
            }
            if (octet == 0 && i > 0U && host[i - 1U] == '0') {
                return false;
            }
            if (octet > 25) {
                return false;
            }
            octet = octet * 10 + (ch - '0');
            if (octet > 255) {
                return false;
            }
        }
    }

    return dots == 3;
}

static bool parse_dns_host(const char *host, size_t len)
{
    size_t label_len = 0;

    if (len == 0U || len > 253U || host[0] == '.' || host[len - 1U] == '.') {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        char ch = host[i];

        if (ch == '.') {
            if (label_len == 0U || label_len > 63U) {
                return false;
            }
            label_len = 0;
        } else {
            if (!char_is_unreserved(ch)) {
                return false;
            }
            ++label_len;
        }
    }

    return label_len > 0U && label_len <= 63U;
}

static bool parse_host(const char *host, size_t len)
{
    bool numeric_shape = len > 0U;

    if (len == 0U) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!(char_is_digit(host[i]) || host[i] == '.')) {
            numeric_shape = false;
            break;
        }
    }

    return numeric_shape ? parse_ipv4(host, len) : parse_dns_host(host, len);
}

static bool parse_port(const char *port, size_t len, uint16_t *out)
{
    uint32_t value = 0;

    if (len == 0U) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!char_is_digit(port[i])) {
            return false;
        }
        value = value * 10U + (uint32_t)(port[i] - '0');
        if (value > 65535U) {
            return false;
        }
    }
    if (value == 0U) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool path_allowed(uri_scheme_t scheme, const char *path_start,
                         size_t total_len, size_t path_offset)
{
    size_t path_len = total_len - path_offset;

    if (scheme == URI_SCHEME_MQTT || scheme == URI_SCHEME_MQTTS) {
        return path_len == 0U;
    }

    if (path_len == 0U || path_start[0] != '/') {
        return false;
    }
    for (size_t i = 0; i < path_len; ++i) {
        unsigned char ch = (unsigned char)path_start[i];

        if (ch == '#' || ch == ' ' || ch < 0x20U || ch == 0x7fU) {
            return false;
        }
        if (ch == '%' &&
            (i + 2U >= path_len ||
             !char_is_hex_safe(path_start[i + 1U]) ||
             !char_is_hex_safe(path_start[i + 2U]))) {
            return false;
        }
    }
    return true;
}

iot_mqtt_uri_error_t iot_mqtt_uri_validate(const char *uri, size_t capacity)
{
    size_t len = 0;
    size_t scheme_end = 0;
    size_t authority_start;
    size_t authority_end;
    size_t host_start;
    size_t host_end;
    size_t port_start;
    size_t port_len;
    uri_scheme_t scheme;
    uint16_t port = 0;
    bool has_port = false;

    if (uri == NULL) {
        return IOT_MQTT_URI_ERR_NULL;
    }
    if (capacity == 0U || capacity > IOT_MQTT_URI_MAX_LEN + 1U) {
        return IOT_MQTT_URI_ERR_TOO_LONG;
    }
    while (len < capacity && uri[len] != '\0') {
        ++len;
    }
    if (len == capacity || len > IOT_MQTT_URI_MAX_LEN) {
        return IOT_MQTT_URI_ERR_TOO_LONG;
    }

    while (scheme_end < len && uri[scheme_end] != ':') {
        ++scheme_end;
    }
    if (scheme_end >= len - 2U || uri[scheme_end + 1U] != '/' ||
        uri[scheme_end + 2U] != '/' ||
        !parse_scheme(uri, scheme_end, &scheme)) {
        return IOT_MQTT_URI_ERR_UNSUPPORTED_SCHEME;
    }

    authority_start = scheme_end + 3U;
    authority_end = authority_start;
    while (authority_end < len && uri[authority_end] != '/' &&
           uri[authority_end] != '?' && uri[authority_end] != '#') {
        ++authority_end;
    }
    if (authority_end == authority_start) {
        return IOT_MQTT_URI_ERR_HOST;
    }
    if (!path_allowed(scheme, uri + authority_end, len, authority_end)) {
        return IOT_MQTT_URI_ERR_PATH;
    }

    host_start = authority_start;
    port_start = authority_end;
    for (size_t i = authority_start; i < authority_end; ++i) {
        if (uri[i] == '@') {
            return IOT_MQTT_URI_ERR_USERINFO;
        }
        if (uri[i] == '[' || uri[i] == ']') {
            return IOT_MQTT_URI_ERR_HOST;
        }
        if (uri[i] == ':') {
            if (has_port) {
                return IOT_MQTT_URI_ERR_HOST;
            }
            has_port = true;
            port_start = i + 1U;
        }
    }

    host_end = has_port ? port_start - 1U : authority_end;
    if (!parse_host(uri + host_start, host_end - host_start)) {
        return IOT_MQTT_URI_ERR_HOST;
    }

    if (has_port) {
        port_len = authority_end - port_start;
        if (!parse_port(uri + port_start, port_len, &port)) {
            return IOT_MQTT_URI_ERR_PORT;
        }
    }

    (void)port;
    return IOT_MQTT_URI_OK;
}
