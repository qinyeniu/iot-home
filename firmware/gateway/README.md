# IoT-Home Gateway Firmware

ESP32-C6 网关固件，实现 Zigbee 协调器 + Wi-Fi STA + MQTT 客户端 + OLED 状态显示。

## 功能特性

- ✅ Wi-Fi STA 连接
- ✅ MQTT 客户端（数据上传和命令接收）
- ⏳ Zigbee 协调器（待实现）
- ⏳ OLED 状态显示（待实现）
- ⏳ 命令转发（待实现）

## 硬件要求

- ESP32-C6 开发板（SuperMini 或 DevKitM-1）
- USB-C 数据线
- 电脑（Windows/Linux/macOS）

## 软件要求

- ESP-IDF v5.5.4（已安装）
- Python 3.11+（已安装）

## 编译步骤

### 1. 打开 ESP-IDF 终端

在 Windows 上，打开开始菜单，找到：
- ESP-IDF v5.5.4 → ESP-IDF PowerShell

或者手动设置环境变量：
`powershell
cd C:\Espressif\v5.5.4\esp-idf
.\export.ps1
`

### 2. 进入固件目录

`powershell
cd C:\Users\HJB\Documents\iot-home\firmware\gateway
`

### 3. 配置项目

`powershell
idf.py menuconfig
`

在配置菜单中：
- Component config → ESP WiFi → WiFi SSID（设置你的 WiFi 名称）
- Component config → ESP WiFi → WiFi Password（设置你的 WiFi 密码）

### 4. 编译固件

`powershell
idf.py build
`

### 5. 烧录固件

连接 ESP32-C6 开发板，然后运行：

`powershell
idf.py -p COM3 flash
`

（将 COM3 替换为你的串口名称）

### 6. 查看日志

`powershell
idf.py -p COM3 monitor
`

## 配置说明

### Wi-Fi 配置

在 sdkconfig.defaults 中修改：

`
CONFIG_ESP_WIFI_SSID="your_wifi_ssid"
CONFIG_ESP_WIFI_PASSWORD="your_wifi_password"
`

### MQTT 配置

在 main/main.c 中修改：

`c
#define MQTT_BROKER_URI "mqtt://localhost:1883"
#define MQTT_TOPIC_PREFIX "iot-home/gw-001"
`

## MQTT 主题

| 主题 | 说明 |
|------|------|
| iot-home/gw-001/nodes/+/telemetry | 遥测数据上报 |
| iot-home/gw-001/nodes/+/status | 设备状态上报 |
| iot-home/gw-001/nodes/+/cmd | 命令下发 |

## 故障排除

### 1. 编译失败

确保 ESP-IDF 环境已正确设置：
`powershell
cd C:\Espressif\v5.5.4\esp-idf
.\export.ps1
`

### 2. 烧录失败

检查串口连接：
`powershell
idf.py -p COM3 flash
`

如果失败，尝试按住 BOOT 按钮再插入 USB。

### 3. Wi-Fi 连接失败

检查 Wi-Fi 名称和密码是否正确。

### 4. MQTT 连接失败

确保 MQTT 服务器正在运行：
`powershell
docker compose ps mosquitto
`

## 下一步开发

1. ✅ Wi-Fi STA 连接
2. ✅ MQTT 客户端
3. ⏳ Zigbee 协调器
4. ⏳ OLED 状态显示
5. ⏳ 命令转发
6. ⏳ 断线重连
7. ⏳ OTA 更新

## 参考资源

- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c6/get-started/)
- [ESP-Zigbee-SDK](https://github.com/espressif/esp-zigbee-sdk)
- [MQTT 协议](https://mqtt.org/)


## WiFi 凭据配置（2026-09-10 起）

真实 WiFi 名称和密码不再写入 Git。首次构建前复制模板：

```powershell
Copy-Item main/wifi_secrets.h.example main/wifi_secrets.h
```

然后编辑 `main/wifi_secrets.h`，填入一个 2.4GHz SSID 和密码。该文件已被 `.gitignore` 忽略。

## MQTT 凭据配置（2026-09-16 起）

```powershell
Copy-Item main/mqtt_secrets.h.example main/mqtt_secrets.h
```

然后编辑 `main/mqtt_secrets.h`，填入与服务器 `server/.env` 一致的 MQTT 用户名和密码。该文件已被 `.gitignore` 忽略，不能提交。Broker 关闭匿名访问前，必须先烧录携带该凭据的网关固件。

若 OLED 显示 `WIFI:NO`，先看 `docs/GATEWAY_WIFI_RECOVERY.md`，不要直接判断为 MQTT 或 OLED 故障。
