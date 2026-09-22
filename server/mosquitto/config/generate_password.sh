#!/bin/sh
# Generate Mosquitto password and ACL files from environment variables.
#
# Supported modes:
# 1) Least-privilege split accounts (recommended):
#      MQTT_BACKEND_USER / MQTT_BACKEND_PASSWORD  (backend service)
#      MQTT_GATEWAY_USER / MQTT_GATEWAY_PASSWORD  (gateway firmware)
#      MQTT_GATEWAY_ID                             (default: gw-001)
# 2) Migration mode:
#      split accounts plus MQTT_ENABLE_LEGACY_ACCOUNT=true and the old
#      MQTT_USER / MQTT_PASSWORD account. This is a short coexistence window.
# 3) Legacy single shared account (development/rollback only):
#      MQTT_USER / MQTT_PASSWORD with readwrite on the whole prefix.
set -eu

: "${MQTT_TOPIC_PREFIX:?MQTT_TOPIC_PREFIX is required}"
PASSWORD_FILE="${MQTT_PASSWORD_FILE:-/mosquitto/data/password.txt}"
ACL_FILE="${MQTT_ACL_FILE:-/mosquitto/data/acl.conf}"
GATEWAY_ID="${MQTT_GATEWAY_ID:-gw-001}"
ENABLE_LEGACY="${MQTT_ENABLE_LEGACY_ACCOUNT:-false}"

case "$ENABLE_LEGACY" in
  true|false) ;;
  *)
    echo "MQTT_ENABLE_LEGACY_ACCOUNT must be 'true' or 'false'" >&2
    exit 1
    ;;
esac

fail() {
    echo "$*" >&2
    exit 1
}

validate_user() {
    name="$1"
    value="$2"

    case "$value" in
        "") fail "$name must not be empty" ;;
        *[!A-Za-z0-9_.-]*)
            fail "$name may contain only letters, digits, underscore, dot or hyphen"
            ;;
    esac
    [ "${#value}" -le 64 ] || fail "$name must be 64 characters or shorter"
}

validate_gateway_id() {
    case "$GATEWAY_ID" in
        "") fail "MQTT_GATEWAY_ID must not be empty" ;;
        *[!A-Za-z0-9_-]*)
            fail "MQTT_GATEWAY_ID may contain only letters, digits, underscore or hyphen"
            ;;
    esac
    [ "${#GATEWAY_ID}" -le 64 ] || fail "MQTT_GATEWAY_ID must be 64 characters or shorter"
}

validate_prefix() {
    case "$MQTT_TOPIC_PREFIX" in
        "") fail "MQTT_TOPIC_PREFIX must not be empty" ;;
        *#|*+|*/|/*|*[!A-Za-z0-9_/-]*)
            fail "MQTT_TOPIC_PREFIX must be slash-separated literals without wildcards"
            ;;
    esac
    case "$MQTT_TOPIC_PREFIX" in
        *//*) fail "MQTT_TOPIC_PREFIX must not contain empty path segments" ;;
    esac
    [ "${#MQTT_TOPIC_PREFIX}" -le 128 ] || fail "MQTT_TOPIC_PREFIX must be 128 characters or shorter"

    old_ifs="$IFS"
    IFS=/
    # Intentional word splitting: split the configured topic prefix.
    for segment in $MQTT_TOPIC_PREFIX; do
        [ -n "$segment" ] || fail "MQTT_TOPIC_PREFIX must not contain empty path segments"
    done
    IFS="$old_ifs"
}

validate_strong_password() {
    name="$1"
    value="$2"

    case "$value" in
        "") fail "$name must not be empty" ;;
        *[[:cntrl:]]*) fail "$name must not contain control characters" ;;
    esac
    [ "${#value}" -ge 12 ] || fail "$name must be at least 12 characters long"

    lower_value=$(printf '%s' "$value" | tr 'A-Z' 'a-z')
    case "$lower_value" in
        *changeme*|*password*|*改成*)
            fail "$name appears to be a placeholder; use a unique strong password"
            ;;
    esac
}

add_password() {
    file="$1"
    user="$2"
    password="$3"
    mode="$4"

    # Feed passwords on stdin so secrets do not appear in the process list.
    if [ "$mode" = "create" ]; then
        printf '%s\n%s\n' "$password" "$password" |
            mosquitto_passwd -c "$file" "$user"
    else
        printf '%s\n%s\n' "$password" "$password" |
            mosquitto_passwd "$file" "$user"
    fi
}

append_legacy_acl() {
    cat >> "$ACL_FILE" <<EOF
# Temporary legacy shared account: full read/write under the deployment prefix.
# Remove this by setting MQTT_ENABLE_LEGACY_ACCOUNT=false after migration.
user $MQTT_USER
topic readwrite $MQTT_TOPIC_PREFIX/#
EOF
}

add_legacy_account() {
    : "${MQTT_USER:?MQTT_USER is required}"
    : "${MQTT_PASSWORD:?MQTT_PASSWORD is required}"
    validate_user MQTT_USER "$MQTT_USER"
    # During migration the legacy account is still a privileged account.
    validate_strong_password MQTT_PASSWORD "$MQTT_PASSWORD"
    add_password "$PASSWORD_FILE" "$MQTT_USER" "$MQTT_PASSWORD" append
    append_legacy_acl
}

write_legacy_only() {
    : "${MQTT_USER:?MQTT_USER is required}"
    : "${MQTT_PASSWORD:?MQTT_PASSWORD is required}"

    echo "WARNING: using legacy single MQTT account; prefer split accounts" >&2
    rm -f "$PASSWORD_FILE"
    validate_user MQTT_USER "$MQTT_USER"
    add_password "$PASSWORD_FILE" "$MQTT_USER" "$MQTT_PASSWORD" create
    cat > "$ACL_FILE" <<EOF
# Legacy shared account: full read/write under the deployment prefix.
user $MQTT_USER
topic readwrite $MQTT_TOPIC_PREFIX/#
EOF
}

write_split_accounts() {
    : "${MQTT_BACKEND_USER:?MQTT_BACKEND_USER is required}"
    : "${MQTT_BACKEND_PASSWORD:?MQTT_BACKEND_PASSWORD is required}"
    : "${MQTT_GATEWAY_USER:?MQTT_GATEWAY_USER is required}"
    : "${MQTT_GATEWAY_PASSWORD:?MQTT_GATEWAY_PASSWORD is required}"

    validate_user MQTT_BACKEND_USER "$MQTT_BACKEND_USER"
    validate_user MQTT_GATEWAY_USER "$MQTT_GATEWAY_USER"
    validate_strong_password MQTT_BACKEND_PASSWORD "$MQTT_BACKEND_PASSWORD"
    validate_strong_password MQTT_GATEWAY_PASSWORD "$MQTT_GATEWAY_PASSWORD"
    [ "$MQTT_BACKEND_USER" != "$MQTT_GATEWAY_USER" ] ||
        fail "MQTT backend and gateway users must differ"

    rm -f "$PASSWORD_FILE"
    add_password "$PASSWORD_FILE" "$MQTT_BACKEND_USER" "$MQTT_BACKEND_PASSWORD" create
    add_password "$PASSWORD_FILE" "$MQTT_GATEWAY_USER" "$MQTT_GATEWAY_PASSWORD" append

    # Topic shape:
    #   {prefix}/{gateway}/nodes/{node}/telemetry|status|rf_policy
    #   {prefix}/{gateway}/nodes/{node}/cmd
    cat > "$ACL_FILE" <<EOF
# Backend: read all device reports, write device commands only.
user $MQTT_BACKEND_USER
topic read $MQTT_TOPIC_PREFIX/+/nodes/+/telemetry
topic read $MQTT_TOPIC_PREFIX/+/nodes/+/status
topic read $MQTT_TOPIC_PREFIX/+/nodes/+/rf_policy
topic write $MQTT_TOPIC_PREFIX/+/nodes/+/cmd

# Gateway firmware: publish reports for its own gateway id and subscribe only
# to commands addressed to nodes under that gateway.
user $MQTT_GATEWAY_USER
topic write $MQTT_TOPIC_PREFIX/$GATEWAY_ID/nodes/+/telemetry
topic write $MQTT_TOPIC_PREFIX/$GATEWAY_ID/nodes/+/status
topic write $MQTT_TOPIC_PREFIX/$GATEWAY_ID/nodes/+/rf_policy
topic read $MQTT_TOPIC_PREFIX/$GATEWAY_ID/nodes/+/cmd
EOF

    if [ "$ENABLE_LEGACY" = "true" ]; then
        [ "$MQTT_USER" != "$MQTT_BACKEND_USER" ] ||
            fail "Legacy user must differ from MQTT backend user"
        [ "$MQTT_USER" != "$MQTT_GATEWAY_USER" ] ||
            fail "Legacy user must differ from MQTT gateway user"
        add_legacy_account
        echo "WARNING: legacy MQTT account enabled for migration; remove it after firmware rollout" >&2
    fi
}

validate_prefix
validate_gateway_id
if [ -n "${MQTT_BACKEND_USER:-}" ] || [ -n "${MQTT_GATEWAY_USER:-}" ]; then
    write_split_accounts
else
    [ "$ENABLE_LEGACY" = "false" ] ||
        fail "MQTT_ENABLE_LEGACY_ACCOUNT=true has no effect unless split accounts are configured"
    write_legacy_only
fi

chmod 600 "$PASSWORD_FILE" "$ACL_FILE"
echo "Mosquitto password and ACL files generated"
