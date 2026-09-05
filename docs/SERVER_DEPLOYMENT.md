# IoT-Home 服务器部署文档

## 服务器信息

- 公网IP: 8.163.110.27
- 用户名: root
- 密码: 20051030hjbHJB.

## 登录方式

### SSH 登录（免密）
ssh root@8.163.110.27
注意：免密登录，直接就是 root 用户，不需要 sudo su

### VNC 登录
- 通过阿里云控制台 VNC 连接
- 用户名：root
- 密码：20051030hjbHJB.

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
- MQTT: iot_user / 20051030hjbHJB
- MySQL: iot_home / 20051030hjbHJB
- Grafana: admin / 20051030hjbHJB

## 启动服务

cd /root/iot-home/server
cp docker-compose-minimal.yml docker-compose.yml
docker compose up -d

## 停止服务

cd /root/iot-home/server
docker compose down

## 查看状态

docker compose ps
docker stats
