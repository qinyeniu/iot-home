# IoT-Home 开发指南

## 快速开始

### 1. 环境准备

**前置要求**:
- Windows 10/11
- Docker Desktop 已安装
- Python 3.11+ (用于模拟设备)
- Git

**检查环境**:
`powershell
# 检查 Docker
docker --version
docker compose --version

# 检查 Python
python --version

# 检查 Git
git --version
`

### 2. 启动服务

**方法一: 使用启动脚本 (推荐)**
`ash
cd server
start.bat
`

**方法二: 手动启动**
`ash
# 1. 启动 Docker Desktop (手动)

# 2. 进入 server 目录
cd server

# 3. 启动所有服务
docker compose up -d

# 4. 查看服务状态
docker compose ps

# 5. 查看日志
docker compose logs -f
`

### 3. 验证服务

**检查服务状态**:
`ash
# 查看运行中的容器
docker compose ps

# 应该看到 4 个容器:
# - iot-mosquitto (MQTT)
# - iot-mysql (MySQL)
# - iot-backend (FastAPI)
# - iot-grafana (Grafana)
`

**访问服务**:
- FastAPI 后端: http://localhost:8000
- API 文档: http://localhost:8000/docs
- Grafana 看板: http://localhost:3000

### 4. 运行模拟设备

**安装依赖**:
`ash
pip install paho-mqtt
`

**启动模拟设备**:
`ash
python tools/simulator.py
`

**预期输出**:
`
🚀 IoT-Home 模拟设备启动
📡 网关: gw-001
📱 节点: 3 个

🔗 正在连接 localhost:1883...
✅ 已连接到 MQTT 服务器: localhost:1883
📤 发送设备上线状态...
  ✅ 客厅温湿度传感器 (sensor-01) 已上线
  ✅ 卧室温湿度传感器 (sensor-02) 已上线
  ✅ 客厅灯光开关 (switch-01) 已上线

📊 开始发送模拟数据...
  📈 客厅温湿度传感器: temperature=24.5, humidity=55.2, light=487
  📈 卧室温湿度传感器: temperature=23.8, humidity=58.1
  📈 客厅灯光开关: status=1
`

### 5. 查看数据

**API 文档**:
- 打开 http://localhost:8000/docs
- 尝试调用 /api/devices 接口
- 尝试调用 /api/metrics/latest 接口

**Grafana 看板**:
- 打开 http://localhost:3000
- 登录: admin / grafana_2024
- 查看 "IoT-Home 环境监测" 看板

## 开发工作流

### 后端开发

**代码位置**: server/backend/app/

**修改代码后重启**:
`ash
# 如果使用 Docker Compose
docker compose restart backend

# 或者查看日志
docker compose logs -f backend
`

**本地开发模式** (不使用 Docker):
`ash
# 1. 安装依赖
cd server/backend
pip install -r requirements.txt

# 2. 启动 MySQL 和 Mosquitto
docker compose up -d mysql mosquitto

# 3. 设置环境变量
export MYSQL_HOST=localhost
export MQTT_HOST=localhost

# 4. 启动 FastAPI
uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
`

### 数据库管理

**连接 MySQL**:
`ash
# 使用 Docker exec
docker compose exec mysql mysql -u iot_home -piot_mysql_2024 iot_home

# 或使用 MySQL 客户端
mysql -h localhost -P 3306 -u iot_home -piot_mysql_2024 iot_home
`

**查看表结构**:
`sql
SHOW TABLES;
DESCRIBE devices;
DESCRIBE metrics;
DESCRIBE commands;
`

**查看数据**:
`sql
SELECT * FROM devices;
SELECT * FROM metrics ORDER BY ts DESC LIMIT 10;
SELECT * FROM commands;
`

### MQTT 调试

**订阅所有主题**:
`ash
# 使用 mosquitto_sub
mosquitto_sub -h localhost -p 1883 -u iot_user -P iot_mqtt_2024 -t "iot-home/#" -v

# 或使用 Docker
docker compose exec mosquitto mosquitto_sub -h localhost -u iot_user -P iot_mqtt_2024 -t "iot-home/#" -v
`

**发布测试消息**:
`ash
mosquitto_pub -h localhost -p 1883 -u iot_user -P iot_mqtt_2024 \
  -t "iot-home/gw-001/nodes/sensor-01/telemetry" \
  -m '{"ts":"2026-08-25T12:00:00","data":{"temperature":25.0,"humidity":60.0}}'
`

## API 使用示例

### 获取设备列表

`ash
curl http://localhost:8000/api/devices
`

**响应示例**:
`json
[
  {
    "id": "gw-001-sensor-01",
    "name": "sensor-01",
    "type": "sensor",
    "parent_id": "gw-001",
    "status": "online",
    "last_seen": "2026-08-25T12:00:00",
    "created_at": "2026-08-25T12:00:00"
  }
]
`

### 获取最新指标

`ash
curl http://localhost:8000/api/metrics/latest
`

**响应示例**:
`json
[
  {
    "device_id": "gw-001-sensor-01",
    "metric": "temperature",
    "value": 24.5,
    "ts": "2026-08-25T12:00:00"
  }
]
`

### 发送命令

`ash
curl -X POST http://localhost:8000/api/devices/gw-001-switch-01/commands \
  -H "Content-Type: application/json" \
  -d '{"command": "toggle", "payload": {"state": "on"}}'
`

## 故障排除

### 1. Docker 启动失败

**问题**: docker: error during connect

**解决方案**:
1. 确保 Docker Desktop 已启动
2. 检查 Docker 服务状态: docker info
3. 重启 Docker Desktop

### 2. 端口被占用

**问题**: Bind for 0.0.0.0:3306 failed: port is already allocated

**解决方案**:
`ash
# 查找占用端口的进程
netstat -ano | findstr :3306

# 停止占用端口的进程
taskkill /PID <进程ID> /F

# 或修改 docker-compose.yml 使用其他端口
`

### 3. MySQL 连接失败

**问题**: Can't connect to MySQL server

**解决方案**:
1. 等待 MySQL 完全启动 (约30秒)
2. 检查健康检查: docker compose ps
3. 查看 MySQL 日志: docker compose logs mysql

### 4. MQTT 连接失败

**问题**: Connection refused

**解决方案**:
1. 检查 Mosquitto 服务: docker compose ps mosquitto
2. 验证密码配置: 检查 server/.env
3. 查看 Mosquitto 日志: docker compose logs mosquitto

### 5. Grafana 看板无数据

**问题**: 看板显示 "No data"

**解决方案**:
1. 确认 MySQL 数据源配置正确
2. 运行模拟设备生成测试数据
3. 刷新 Grafana 页面
4. 检查 Grafana 数据源连接: Configuration -> Data Sources

## 下一步开发

### 第 3-4 周: 网关固件

1. 安装 ESP-IDF
2. 创建网关固件项目
3. 实现 Wi-Fi STA 连接
4. 实现 MQTT 客户端
5. 实现 OLED 状态显示
6. 实现断线重连机制

### 第 5-7 周: Zigbee 组网

1. 实现 Zigbee 协调器 (网关)
2. 实现 Zigbee 终端节点 (传感器)
3. 实现 Zigbee 终端节点 (开关)
4. 集成遥测数据上报
5. 实现命令下发

### 第 8-9 周: 可靠性

1. OTA 固件更新
2. 断线重连优化
3. 可选 TLS 加密
4. KiCad 底板设计

## 参考资源

- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c6/get-started/)
- [ESP-Zigbee-SDK](https://github.com/espressif/esp-zigbee-sdk)
- [FastAPI 文档](https://fastapi.tiangolo.com/)
- [Mosquitto 文档](https://mosquitto.org/documentation/)
- [Grafana 文档](https://grafana.com/docs/)
