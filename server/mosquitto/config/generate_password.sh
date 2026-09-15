#!/bin/sh
# Generate a Mosquitto password file from environment variables.
# Example: MQTT_USER=iot_user MQTT_PASSWORD='...' ./generate_password.sh
set -eu

: "${MQTT_USER:?MQTT_USER is required}"
: "${MQTT_PASSWORD:?MQTT_PASSWORD is required}"

mosquitto_passwd -c -b /mosquitto/config/password.txt "$MQTT_USER" "$MQTT_PASSWORD"
chmod 600 /mosquitto/config/password.txt

echo "MQTT password file generated"
