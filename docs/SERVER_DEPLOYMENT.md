# IoT-Home 服务器部署文档

## 服务器信息

- 公网IP: 8.163.110.27
- 用户名: root
- 密码: 见仓库外凭据保存位置；不要写入仓库（建议使用 SSH key）

## 登录方式

### SSH 登录（免密）
ssh root@8.163.110.27
注意：免密登录，直接就是 root 用户，不需要 sudo su

### VNC 登录
- 通过阿里云控制台 VNC 连接
- 用户名：root
- 密码：见仓库外凭据保存位置；不要写入仓库（建议使用 SSH key）

## 项目信息

### IoT-Home 项目
- 仓库地址：https://github.com/qinyeniu/iot-home.git
- 项目路径：/root/iot-home
- 配置文件：/root/iot-home/server/docker-compose-minimal.yml

### 端口分配
- Mosquitto: 1883（MQTT TCP）、9001（WebSocket；明文端口不应直接暴露公网）
- MySQL: 3307
- FastAPI: 8000
- Grafana: 3000
- 公网生产前需增加 TLS/WSS、VPN 或云防火墙白名单

### 登录信息

用户名保留在本节，真实密码统一放在服务器的 `/root/iot-home/server/.env`，不要写入文档或 Git：

- MQTT 用户名: iot_user
- MySQL 用户名: iot_home
- Grafana 用户名: admin

## 启动服务

cd /root/iot-home/server
test -f .env || cp .env.example .env  # 首次部署：编辑 .env 填入真实值
docker compose -f docker-compose-minimal.yml up -d


## MQTT 认证迁移

新版本会在 Mosquitto 容器启动时从 `.env` 生成密码和 ACL。迁移过程中**不需要打开匿名访问**，避免公网设备被未授权连接。

1. 在服务器 `.env` 中配置后端/网关分离账号，并临时设置 `MQTT_ENABLE_LEGACY_ACCOUNT=true`；
2. 重建 Mosquitto，使新账号和旧 `MQTT_USER` 账号可以共存；
3. 确认 FastAPI 使用后端独立账号连接、订阅和发布命令正常；
4. 烧录包含网关独立账号的新固件，确认 MQTT、遥测和下行命令正常；
5. 将 `MQTT_ENABLE_LEGACY_ACCOUNT=false` 并再次重建 Mosquitto，移除旧账号；
6. 全程保持 `MQTT_ALLOW_ANONYMOUS=false`；
7. 验证匿名连接被拒绝、跨网关访问被拒绝，API、Grafana 和网关遥测均正常。

完整说明见 `docs/MQTT_SECURITY_HARDENING.md`。

## 停止服务

cd /root/iot-home/server
docker compose down

## 查看状态

docker compose ps
docker stats
