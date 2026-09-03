# iot-home — IoT 三层架构学习原型

一个"终端 + 网关 + 服务器"完整三层架构的项目原型，作为后续扩展的基座。
第一版主题：**室内环境监测 + 远程开关**。主题不重要，核心要求是**扩展性**：
以后增加新传感器、新执行器、新终端节点时，不需要重构现有代码和数据库。

## 技术选型

| 层 | 方案 |
|----|------|
| 终端 / 网关 | ESP32-C6（内置 Wi-Fi 6 + BLE + Zigbee 3.0/Thread） |
| 终端 ↔ 网关 | Zigbee 3.0（乐鑫 ESP-Zigbee-SDK，标准 ZCL 簇） |
| 网关上联 | Wi-Fi STA + MQTT |
| 消息中间件 | Mosquitto（MQTT） |
| 服务端 | FastAPI + MySQL 8.0（独立库 iot_home）+ Grafana |
| 本地开发 | Windows + Docker Compose 一键环境（Mosquitto + MySQL + FastAPI + Grafana） |
| 服务器部署 | 阿里云 ECS，systemd 管理（**已后置**，等服务器资源优化） |

## 快速开始

### 1. 启动 Docker Desktop

确保 Docker Desktop 已启动并运行。

### 2. 启动服务

`ash
cd server
start.bat
`

### 3. 运行模拟设备

`ash
pip install paho-mqtt
python tools/simulator.py
`

### 4. 访问服务

| 服务 | 地址 | 说明 |
|------|------|------|
| FastAPI 后端 | http://localhost:8000 | REST API |
| API 文档 | http://localhost:8000/docs | Swagger UI |
| Grafana 看板 | http://localhost:3000 | 数据可视化 |
| MQTT | localhost:1883 | 消息中间件 |
| MySQL | localhost:3306 | 数据库 |

### 5. 默认登录信息

| 服务 | 用户名 | 密码 |
|------|--------|------|
| Grafana | admin | grafana_2024 |
| MySQL | iot_home | iot_mysql_2024 |
| MQTT | iot_user | iot_mqtt_2024 |

## 目录结构

`
iot-home/
├── docs/                          # 分阶段中文文档（设计先行）
│   ├── PROJECT_STATUS.md          # 项目状态文档
│   ├── DEVELOPMENT_GUIDE.md       # 开发指南
│   └── hardware/                  # 硬件设计文档
├── firmware/                      # 固件（ESP32-C6）
│   ├── gateway/                   # 网关固件：Zigbee 协调器 + Wi-Fi + MQTT + OLED
│   ├── node_sensor/               # 传感器终端：AHT20 + BH1750 + SSD1306
│   ├── node_switch/               # 开关终端：继电器 / WS2812B
│   └── common/                    # 共享模块：传感器驱动注册表、MQTT 主题常量
├── server/                        # 服务端
│   ├── backend/                   # FastAPI 后端：MQTT 订阅入库 + 设备/命令 API
│   ├── docker-compose.yml         # 本地一键环境
│   ├── grafana/                   # 数据源与看板 provisioning
│   ├── mysql/init/                # 建库建表初始化脚本
│   ├── mosquitto/config/          # MQTT 配置
│   └── .env.example               # 环境变量模板
├── hardware/                      # 接线图、BOM、KiCad 底板图纸
└── tools/                         # 模拟器与调试工具（硬件到货前联调用）
`

## 阶段计划

| 阶段 | 内容 | 状态 |
|------|------|------|
| 阶段 0：硬件先行 | 可行性分析 → 采购确认 → 到货验收（串口 / I2C 逐模块实测） | ✅ 完成 |
| 第 1–2 周 | 本地 Docker 端到端：模拟设备 → MQTT → 入库 → Grafana 曲线 | ✅ 完成 |
| 第 3–4 周 | 网关固件：ESP-IDF + Wi-Fi + MQTT + OLED + 断线重连（实机测试驱动） | ⏳ 待开始 |
| 第 5–7 周 | Zigbee 组网、传感器节点、开关节点；Grafana + 远程控制接口（实机测试驱动） | ⏳ 待开始 |
| 第 8–9 周 | 可靠性（OTA、重连、可选 TLS）+ KiCad 底板 → 嘉立创 SMT 贴片 | ⏳ 待开始 |
| 第 10 周起 | 按兴趣扩展：低功耗电池、更多传感器、告警推送、Matter、Web App | ⏳ 待开始 |

## API 接口

### 设备管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/devices | 获取设备列表 |
| GET | /api/devices/{id} | 获取单个设备详情 |
| GET | /api/devices/{id}/metrics | 获取设备指标数据 |
| POST | /api/devices/{id}/commands | 发送命令到设备 |

### 指标查询

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/metrics/latest | 获取最新指标数据 |

### 健康检查

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/health | 服务健康检查 |

## MQTT 主题规范

| 主题 | 说明 |
|------|------|
| iot-home/{gateway_id}/nodes/{node_id}/telemetry | 遥测数据上报 |
| iot-home/{gateway_id}/nodes/{node_id}/status | 设备状态上报 |
| iot-home/{gateway_id}/nodes/{node_id}/cmd | 命令下发 |

## 扩展性设计

1. **指标表设计**: device_id + metric + ts + value，新指标无需改表
2. **传感器驱动注册表**: 新增传感器只加文件，不改主逻辑
3. **I2C 扩展排针**: 3V3/GND/SDA/SCL + 2个备用 GPIO
4. **JSON 公共字段 + extras**: 服务器接口不随传感器种类变化

## 安全约定

- 所有凭据（MQTT 密码、MySQL 密码、服务器密码、AI API Key）只放 .env，严禁写入代码、文档或提交仓库。
- .env 已加入 .gitignore；仓库只提交 .env.example 占位模板。

## 工作方式

- 全程中文沟通；设计先行，每阶段先给简明文档再写代码。
- 每交付一个阶段先自测/自查，再给出可复现的操作步骤。
- 每一步完成后先汇报，经确认再进入下一步。

### 硬件实测驱动（本项目核心原则）

- **先硬件后代码**：任何功能模块必须先通过实机测试（串口日志、传感器读数、命令响应、稳定性观察）才算完成；"理论上应该能用"只是待验证假设，不算完成。
- **拒绝未经实测的假设**：我提出的每个设计点，都必须在硬件上跑出效果后，才进入"完整功能模块"清单。可行性文档中所有"待实测"项都不作为设计依据。
- **串口联调**：设备通过 USB-C 接到电脑后，由我直接读取 COM 口日志与测试数据辅助开发；工具链（esptool、pyserial 等）在 	ools/ 文档中维护。
- **采购门槛**：硬件采购前必须先过可行性分析并经用户确认；到货后按"到货验收流程"逐模块验证，通过后才进入固件开发。

## 文档索引

| 文档 | 说明 |
|------|------|
| [PROJECT_STATUS.md](docs/PROJECT_STATUS.md) | 项目状态文档 |
| [DEVELOPMENT_GUIDE.md](docs/DEVELOPMENT_GUIDE.md) | 开发指南 |
| [硬件可行性分析](docs/hardware/01-硬件可行性分析与采购清单.md) | 硬件选型与采购 |
| [接线图](docs/hardware/02-接线图与首次上电.md) | 硬件接线指南 |
| [面包板入门](docs/hardware/03-面包板入门与接线详解.md) | 面包板使用教程 |
| [排针焊接入门](docs/hardware/04-排针焊接入门.md) | 焊接教程 |
| [ESP-IDF 工具链安装](docs/ESP-IDF工具链安装.md) | ESP-IDF 安装指南 |

## 贡献指南

1. Fork 本仓库
2. 创建功能分支: git checkout -b feature/your-feature
3. 提交更改: git commit -m 'Add some feature'
4. 推送分支: git push origin feature/your-feature
5. 提交 Pull Request

## 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件

## 联系方式

- 项目主页: https://github.com/yourusername/iot-home
- 问题反馈: https://github.com/yourusername/iot-home/issues
