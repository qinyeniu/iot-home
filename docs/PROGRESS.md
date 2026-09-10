# IoT-Home 项目进度文档

**最后更新**: 2026-09-10
**当前阶段**: OLED 已实机目视通过；网关 WiFi 自愈已增强，等待路由器 2.4G 恢复后做 10 分钟稳定观察；Zigbee 端到端待 COM5 烧录验证

## 一、已完成

### 阶段 0：硬件先行 ✅
- 可行性分析与采购清单
- 硬件到货验收
- I2C 设备扫描验证

### 第 1-2 周：本地 Docker 端到端 ✅
- Docker 环境配置
- MQTT、MySQL、FastAPI、Grafana

### 第 3-4 周：网关固件 ✅
- Wi-Fi STA 连接
- MQTT 客户端
- OLED 状态显示（黄蓝双色）
- 遥测数据上报

### 第 5 周：综合终端 ✅
- AHT20 温湿度传感器
- BH1750 光照传感器
- 继电器控制（GPIO20）
- Wi-Fi + MQTT 数据上报

### 第 5-7 周：Zigbee 组网 ✅
- 网关协调器 + 终端入网（addr=0x0e8a）
- 设备加入 MQTT 通知


### 2026-09-10 晚间：OLED 修复与网关 WiFi 自愈增强
- 官方 `espressif/ssd1306` 1.0.5 文字 API 在本机实机乱码；改为网关内本地 SSD1306 5x7 文本驱动，用户目视确认正常。
- WiFi 改为全信道扫描、配置仅 RAM 保存，并打印底层断线 reason/rssi。
- 修复启动看门狗 NVS 计数可能永久停在 2 的问题：改为 RTC 魔数计数，重新断电获得新的 2 次热重启预算，拿 IP 后清零。
- 实机曾在热重启后拿到 `192.168.10.112` 并连接 MQTT；当前 2.4G AP 不稳定时仍会报 201/36，需要先恢复路由器 2.4G。
- 详见 `docs/GATEWAY_WIFI_RECOVERY.md`。

### 2026-09-09：Zigbee 标准 ZCL 数据传输 🔄 待实机验证
- 终端 v3.0：EP10 注册 Basic/Identify + 温度(0x0402)/湿度(0x0405)/照度(0x0400)
  测量 server cluster；每 10s 读传感器并向协调器 0x0000 单播 3 条 Report Attributes
  （温湿度 0.01 单位，照度按 Zigbee 对数刻度 10000*log10(lux+1)）
- 网关 v3.1：EP10 注册 Home Gateway device + 三个测量 client cluster；
  注册统一核心回调 ESP_ZB_CORE_REPORT_ATTR_CB_ID（旧回调写了从未注册，已修复）；
  按源短地址缓存、2s 聚合成一条 MQTT；修复转发 JSON 与后端不兼容问题
  （后端只入库 data 字段）；设备加入通知改发 nodes/zb-{addr}/status；
  OLED 底行显示 ZB 节点数
- 两个工程均 idf.py build 通过（esp-zigbee-lib 1.6.0 API 已对照头文件）
- 设计文档: docs/ZIGBEE数据传输设计.md
- ⚠️ 尚未烧录实测，按项目原则不算完成

## 二、当前硬件

| 设备 | 串口 | 说明 |
|------|------|------|
| 网关 ESP32-C6 | COM6 | 协调器 + Wi-Fi + MQTT + OLED |
| Zigbee 终端 ESP32-C6 | COM5 | AHT20 + BH1750，ED 模式 |

- 服务器: API http://8.163.110.27:8000 / Grafana :3000 / MQTT :1883

## 三、下一步实测（Zigbee 端到端验收）

1. 烧录网关: `cd firmware/gateway; idf.py -p COM6 flash monitor`
2. 烧录终端: `cd firmware/node_zigbee; idf.py -p COM5 flash monitor`
3. 验收点：
   - 终端日志: 周期 DATA JSON + 无 ZCL 报错
   - 网关日志: `ZCL report from 0x....: cluster=0x0402/0405/0400`
     和 `MQTT -> nodes/zb-xxxx/telemetry: {"data":{...}}`
   - MQTT 订阅 `iot-home/gw-001/nodes/#` 看到聚合数据
   - 后端日志"遥测数据已保存: gw-001-zb-xxxx"，API/Grafana 可查
   - 连续观察 10 分钟稳定性
4. 实测通过后：深度睡眠低功耗（见 ZIGBEE_LOW_POWER_DESIGN.md）、多终端、命令下发

## 四、参考文档

- Zigbee 数据传输设计: docs/ZIGBEE数据传输设计.md
- 项目总结: docs/PROJECT_SUMMARY.md
- Zigbee 低功耗设计: docs/ZIGBEE_LOW_POWER_DESIGN.md
- 双项目切换: docs/双项目切换指南.md
- OLED 技术文档: docs/hardware/OLED技术文档.md
- 网关开发配置: firmware/gateway/DEV_CONFIG.md
