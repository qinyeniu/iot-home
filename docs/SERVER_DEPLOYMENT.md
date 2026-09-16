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
- Mosquitto: 1883
- MySQL: 3307
- FastAPI: 8000
- Grafana: 3000

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

新版本会在 Mosquitto 容器启动时从 `.env` 生成密码和 ACL。为避免旧网关被立即锁定：

1. 在服务器 `.env` 中临时设置 `MQTT_ALLOW_ANONYMOUS=true`，更新 Compose 和 `mosquitto/config` 后重建 Mosquitto；
2. 确认 FastAPI 使用账号密码连接正常；
3. 烧录包含 `mqtt_secrets.h` 的新网关固件，确认 MQTT OK 且遥测继续入库；
4. 将服务器 `.env` 改为 `MQTT_ALLOW_ANONYMOUS=false`，再次重建 Mosquitto；
5. 验证匿名连接被拒绝、认证连接、API、Grafana 和网关遥测均正常。

## 停止服务

cd /root/iot-home/server
docker compose down

## 查看状态

docker compose ps
docker stats
