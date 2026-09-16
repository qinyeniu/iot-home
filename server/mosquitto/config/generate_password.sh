#!/bin/sh
# Generate Mosquitto password and ACL files from environment variables.
set -eu

: "${MQTT_USER:?MQTT_USER is required}"
: "${MQTT_PASSWORD:?MQTT_PASSWORD is required}"
: "${MQTT_TOPIC_PREFIX:?MQTT_TOPIC_PREFIX is required}"

PASSWORD_FILE="${MQTT_PASSWORD_FILE:-/mosquitto/data/password.txt}"
ACL_FILE="${MQTT_ACL_FILE:-/mosquitto/data/acl.conf}"

mosquitto_passwd -c -b "$PASSWORD_FILE" "$MQTT_USER" "$MQTT_PASSWORD"
chmod 600 "$PASSWORD_FILE"

cat > "$ACL_FILE" <<EOF
user $MQTT_USER
topic readwrite $MQTT_TOPIC_PREFIX/#
EOF
chmod 600 "$ACL_FILE"

echo "Mosquitto password and ACL files generated"
