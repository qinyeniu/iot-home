#!/bin/bash
# 生成 Mosquitto 密码文件
# 在 Docker 容器启动时执行

# 创建密码文件
mosquitto_passwd -c -b /mosquitto/config/password.txt iot_user iot_mqtt_2024

# 设置权限
chmod 600 /mosquitto/config/password.txt

echo "MQTT 密码文件已生成"