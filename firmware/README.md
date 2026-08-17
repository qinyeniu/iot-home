# firmware — 固件（ESP32-C6）

三个固件工程共用 ESP-IDF + ESP-Zigbee-SDK：

| 目录 | 角色 | 职责 |
|------|------|------|
| `gateway/` | 网关 | Zigbee 协调器 + Wi-Fi STA + MQTT 客户端 + OLED 状态显示 |
| `node_sensor/` | 传感器终端 | AHT20 + BH1750 + SSD1306，Zigbee 入网并周期上报 |
| `node_switch/` | 开关终端 | 继电器 / WS2812B，Zigbee 接收服务器下发的开关命令 |
| `common/` | 共享模块 | 传感器驱动注册表、I2C 总线、MQTT 主题常量 |

第 3–4 周开始编写网关固件，第 5–7 周编写终端固件。
