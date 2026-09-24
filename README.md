# iot-home — ESP32-C6 三层架构 IoT 原型

本项目是一个中文、设计先行、硬件实测驱动的智能家居原型，当前主题为：

```text
Zigbee 传感/开关节点 → ESP32-C6 网关 → Wi‑Fi/MQTT → FastAPI/MySQL/Grafana
```

第一版业务目标：**室内环境监测 + 远程开关**；长期目标是支持产品化配网、凭据管理、电池低功耗节点和多节点扩展。

## 1. 当前真实状态

### 已在硬件上验证

- ESP32-C6 Zigbee coordinator 与 end device 组网；
- channel 26，节点 authenticated secure rejoin；
- AHT20 温湿度、BH1750 光照通过标准 ZCL report 上报；
- 网关聚合 Zigbee 数据并发布 MQTT；
- FastAPI 订阅 MQTT 并写入 MySQL；
- Grafana 展示环境数据；
- on/off/toggle 远程控制继电器并回报 OnOff 状态；
- RF QUERY → SET 0 → SET -10 → AUTO 命令链路；
- RF 发送失败后自动退避；
- 正常拔掉节点不会导致网关重启。

### 离线完成，等待硬件维护窗口

- RF policy 相同值不重复写 NVS，降低 Flash 磨损；
- On/Off 陈旧目标保护；
- NVS A/B 完整配置 bundle 与启动路径；
- RF selected node 目标匹配；
- 电池 SED 60 秒采样上报和参数配置化；
- 小规格阿里云 ECS 的资源预算和默认不生效的 Docker 资源保护覆盖文件。

### 明确暂不自动执行

- MQTT ACL 公网部署；
- TLS/WSS、证书、VPN；
- 安全组、DNS、远程容器重启；
- NVS encryption、Flash encryption、Secure Boot；
- bootloader/分区表写入；
- Zigbee NVRAM 擦除；
- 网卡/热点切换。

这些事项需要用户在场，按维护窗口和回退方案执行。

## 2. 技术栈

| 层级 | 技术 |
|---|---|
| 节点 | ESP32-C6、Zigbee End Device、AHT20、BH1750、继电器、本地按键 |
| 网关 | ESP32-C6、Zigbee Coordinator、Wi‑Fi STA、MQTT、OLED SSD1306 |
| 消息 | Mosquitto MQTT |
| 后端 | FastAPI、SQLAlchemy、Paho MQTT |
| 数据库 | MySQL 8 |
| 可视化 | Grafana |
| 本地环境 | Windows、ESP-IDF v5.5.4、Docker Compose |
| 后期服务器 | 阿里云 ECS（小规格，需要保留资源余量） |

## 3. 目录结构

```text
docs/
  HANDOFF/                  # 每日入口摘要索引
  HANDOFF_YYYY-MM-DD.md     # 每日完整交接（核心长期记忆）
  CURRENT_DEVELOPMENT_PLAN.md
  FIRMWARE_FULL_CHAIN_WALKTHROUGH.md
  ALIYUN_SERVER_MAINTENANCE_PLAN.md
firmware/
  gateway/                  # 网关固件
  node_zigbee/              # 现网“传感器 + 继电器”组合节点
  node_battery_sensor/      # 独立电池 SED 实验工程
  common/iot_product_config/
server/
  backend/                  # FastAPI 后端与测试
  mysql/
  mosquitto/
  grafana/
  docker-compose.yml
  docker-compose-resource-guard.yml
tools/
  analyze_long_test.py
  capture_serial*.py
hardware/
```

大量串口日志、构建目录和临时补丁脚本可能处于未跟踪状态；不得使用 `git add -A`。

## 4. 本地服务快速启动

1. 启动 Docker Desktop；
2. 进入 `server/`；
3. 按现有脚本启动本地 Compose；
4. 后端 API 默认在 `http://localhost:8000`；
5. Grafana 默认在 `http://localhost:3000`。

真实数据库、Grafana 或 MQTT 密码只从本地环境文件读取，不要在对话或文档中打印。

## 5. 固件构建

### 网关

```powershell
cd firmware\gateway
. C:\Espressif\v5.5.4\esp-idf\export.ps1
idf.py build
```

### 电池 SED

```powershell
cd firmware\node_battery_sensor
. C:\Espressif\v5.5.4\esp-idf\export.ps1
idf.py build
```

烧录前必须重新确认端口、二进制 SHA256 和应用分区剩余空间。未经明确授权，只能在维护窗口写应用分区；不得刷 bootloader/分区表或擦 Zigbee NVRAM。

## 6. MQTT 主题

| 主题 | 说明 |
|---|---|
| `iot-home/{gateway}/nodes/{node}/telemetry` | 传感器/开关状态 |
| `iot-home/{gateway}/nodes/{node}/status` | 节点 retained 状态 |
| `iot-home/{gateway}/nodes/{node}/cmd` | 命令下发 |
| `iot-home/{gateway}/nodes/{node}/rf_policy` | RF 策略状态 |
| `iot-home/gateway/{gateway}/status` | 网关状态/LWT |

短地址不是稳定产品 ID；后续应以 IEEE address 或安装阶段生成的稳定 ID 做长期映射。

## 7. 测试

本地后端：

```powershell
cd server\backend
.\.venv\Scripts\python.exe -m pytest tests -q
```

长测分析器：

```powershell
.\.venv\Scripts\python.exe -m pytest tools\tests\test_analyze_long_test.py -q
```

历史 MicroPython 测试依赖 `machine`，不能直接用 CPython 全量运行。

## 8. 推荐后续顺序

1. 完成烧录后 8 小时双串口长测并形成最终结论；
2. 用户确认后，小步验证离线固件候选；
3. 验证 NVS bundle save/load/restart、异常 marker 和掉电恢复；
4. 做最小 SoftAP/HTTP，不一次加入完整配网状态机；
5. 教学式部署 MQTT ACL；
6. 用户准备域名后再配置 TLS/WSS；
7. 管理入口按 VPN 或安全组白名单收紧；
8. 电池节点接入负载开关并做电流实测；
9. 关键行为稳定后再拆分大型 `main.c`。

## 9. 工作规则

- 全程中文；
- 设计先行、硬件实测驱动；
- 每次会话/阶段结束必须更新本地交接文档；
- 显式 `git add` 相关文件，不使用 `git add -A`；
- 不 push、不部署，除非用户明确要求；
- 不记录真实 Wi‑Fi、MQTT、数据库密码；
- 不切换用户电脑网卡，除非提前留好恢复路径并获得明确同意。

## 10. 首要阅读文档

1. `docs/HANDOFF/README.md`
2. 最新 `docs/HANDOFF/_YYYY-MM-DD.md`
3. 对应完整交接
4. `docs/CURRENT_DEVELOPMENT_PLAN.md`
5. `docs/FIRMWARE_FULL_CHAIN_WALKTHROUGH.md`
