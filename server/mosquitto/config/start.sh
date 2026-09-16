#!/bin/sh
set -eu

ALLOW_ANONYMOUS="${MQTT_ALLOW_ANONYMOUS:-false}"
case "$ALLOW_ANONYMOUS" in
  true|false) ;;
  *)
    echo "MQTT_ALLOW_ANONYMOUS must be 'true' or 'false'" >&2
    exit 1
    ;;
esac

RUNTIME_CONF="${MQTT_RUNTIME_CONF:-/mosquitto/data/mosquitto.conf}"
PASSWORD_FILE="${MQTT_PASSWORD_FILE:-/mosquitto/data/password.txt}"
ACL_FILE="${MQTT_ACL_FILE:-/mosquitto/data/acl.conf}"

mkdir -p /mosquitto/data
MQTT_PASSWORD_FILE="$PASSWORD_FILE" MQTT_ACL_FILE="$ACL_FILE" \
  sh /mosquitto/config/generate_password.sh

cp /mosquitto/config/mosquitto.conf "$RUNTIME_CONF"
cat >> "$RUNTIME_CONF" <<EOF
allow_anonymous $ALLOW_ANONYMOUS
password_file $PASSWORD_FILE
acl_file $ACL_FILE
EOF
chmod 600 "$RUNTIME_CONF"
if id -u mosquitto >/dev/null 2>&1; then
  chown -R mosquitto:mosquitto /mosquitto/data
fi

exec mosquitto -c "$RUNTIME_CONF"
