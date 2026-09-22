# mosquitto/config — MQTT 配置

- `mosquitto.conf`：MQTT TCP 1883、WebSocket 9001、持久化和日志配置；
- `start.sh` / `generate_password.sh`：容器启动时从 `server/.env` 生成密码、ACL 和运行时认证配置；
- 生成文件位于 Docker volume `/mosquitto/data/`，不进入 Git；
- `MQTT_ALLOW_ANONYMOUS=false` 为最终安全状态；迁移设备凭据时也不需要打开匿名。

## 账号模式

推荐配置分离账号：

- `MQTT_BACKEND_USER` / `MQTT_BACKEND_PASSWORD`：后端读取所有设备上报，只能向 `cmd` 主题写命令；
- `MQTT_GATEWAY_USER` / `MQTT_GATEWAY_PASSWORD`：网关只能向自己的 `telemetry` / `status` / `rf_policy` 发布，并订阅自己的 `cmd`；
- `MQTT_GATEWAY_ID`：网关主题段，当前固件使用 `gw-001`。

迁移阶段可设置：

```env
MQTT_ENABLE_LEGACY_ACCOUNT=true
```

这会在保留后端/网关独立账号的同时，短期保留旧 `MQTT_USER` / `MQTT_PASSWORD`。验证新网关固件和后端均正常后，应改回 `false` 并重建 Broker，删除旧账号。

如果没有配置分离账号，脚本会保留旧行为：`MQTT_USER` / `MQTT_PASSWORD` 对 `MQTT_TOPIC_PREFIX/#` 拥有完整读写权限。该模式只作为开发/回退，不建议长期使用。

## 建议上线顺序

1. 在服务器 `.env` 中增加后端和网关分离账号及强密码，并设置 `MQTT_ENABLE_LEGACY_ACCOUNT=true`；
2. 重建 Mosquitto，此时新旧账号可同时认证；
3. 更新网关固件使用网关账号，确认网关能连接、发布并接收命令；
4. 确认后端独立账号订阅和命令发布正常；
5. 设置 `MQTT_ENABLE_LEGACY_ACCOUNT=false`，再次重建 Mosquitto；
6. 保持 `MQTT_ALLOW_ANONYMOUS=false`，验证匿名连接和越权主题均被拒绝。

## 公网安全

1883/9001 当前是明文协议。若 Broker 暴露在公网，必须先配置 TLS/WSS、VPN 或云防火墙白名单；详见 `docs/MQTT_SECURITY_HARDENING.md`。

## 回退

若新账号迁移失败，可暂时让后端/网关使用旧 `MQTT_USER` / `MQTT_PASSWORD`；不要长期打开匿名访问。
