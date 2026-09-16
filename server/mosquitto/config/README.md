# mosquitto/config — MQTT 配置

- `mosquitto.conf`：基础监听、持久化和日志配置；
- `start.sh` / `generate_password.sh`：容器启动时从 `server/.env` 生成密码、ACL 和运行时认证配置；
- 生成文件位于 Docker volume `/mosquitto/data/`，不进入 Git；
- `MQTT_ALLOW_ANONYMOUS=false` 为最终安全状态；迁移网关固件时可短暂设为 `true`。

建议上线顺序：

1. 先部署启动脚本和密码文件，临时设置 `MQTT_ALLOW_ANONYMOUS=true`；
2. 烧录携带 MQTT 用户名和密码的网关固件，确认后端和网关都能认证；
3. 将 `MQTT_ALLOW_ANONYMOUS=false` 写回服务器 `.env`，重建 Mosquitto；
4. 验证 API、后端 MQTT、网关遥测和下行命令均正常。
