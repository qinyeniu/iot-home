# 网关固件开发规范

**更新时间**: 2026-09-06

## 开发工具

### VSCode ESP-IDF 插件（首选）

**所有编译、烧录、调试操作都通过 VSCode 的 ESP-IDF 插件完成，不使用命令行。**

| 操作 | VSCode 方法 |
|------|-------------|
| 编译 | 点击状态栏 🔧 Build 图标，或 `F1` → `ESP-IDF: Build your Project` |
| 烧录 | 点击状态栏 ⚡ Flash 图标，或 `F1` → `ESP-IDF: Flash Your Project` |
| 监控 | 点击状态栏 📺 Monitor 图标，或 `F1` → `ESP-IDF: Monitor Device` |
| 清理 | `F1` → `ESP-IDF: Full Clean` |
| 配置 | `F1` → `ESP-IDF: SDK Configuration Editor` |

### 问题排查流程

1. **首选**：查阅 ESP-IDF 官方文档
2. **其次**：查看 VSCode ESP-IDF 扩展文档
3. **最后**：在 ESP-IDF 输出面板查看错误日志（`Ctrl+Shift+U` → 选择 ESP-IDF）

**不要**：
- ❌ 自动生成命令行命令查看硬件状态
- ❌ 使用 PowerShell/CMD 执行编译烧录操作
- ❌ 跳过文档直接给解决方案

## 烧录配置

### 正确的烧录方式：UART

```json
{
  "idf.flashType": "UART",
  "idf.portWin": "COM6",
  "idf.flashBaudRate": "115200"
}
```

### 烧录步骤

1. 拔掉 USB 线
2. 按住 BOOT 按钮
3. 插上 USB 线（继续按住 BOOT）
4. 等 3 秒
5. 松开 BOOT 按钮
6. 点击 VSCode 中的 Flash 图标

### 注意事项

- **使用主板直连 USB 口，不要用扩展坞**
- 波特率使用 115200（460800 可能不稳定）
- 如果烧录超时，检查是否正确进入下载模式

## 硬件配置

### ESP32-C6 SuperMini

| 项目 | 值 |
|------|-----|
| COM 口 | COM6 |
| 目标芯片 | ESP32-C6 |
| USB 模式 | USB-Serial/JTAG |

### OLED 显示屏（SSD1306 0.96"）

| 项目 | 值 |
|------|-----|
| I2C 地址 | 0x3C |
| 分辨率 | 128x64 |
| 颜色 | 黄蓝双色（上16行黄色，下48行蓝色）|

**接线：**

| OLED 引脚 | ESP32-C6 引脚 |
|-----------|---------------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO2 |
| SCL | GPIO3 |

**⚠️ 注意：SDA 和 SCL 不能接反，否则 I2C 通信失败！**

### I2C 设备地址

| 设备 | 地址 |
|------|------|
| SSD1306 OLED | 0x3C |
| AHT20 温湿度 | 0x38 |
| BH1750 光照 | 0x23 |

## OLED UI 设计规范

### 黄蓝双色屏布局

```
┌─────────────────────────────┐
│      黄色区域 (0-15px)       │  ← 标题居中
│      "IoT-Home GW"          │
├─────────────────────────────┤  ← 分界线 (16px)
│      蓝色区域 (16-63px)      │  ← 详细信息
│  IP: 192.168.137.226        │
│  WiFi:OK  MQTT:OK           │
│  Up: 00:05:30               │
│  GW: gw-001                 │
└─────────────────────────────┘
```

### 设计原则

1. **黄色区域**：只放标题，居中显示
2. **蓝色区域**：放详细信息，左对齐
3. **字体**：5x7 像素字体，每个字符 6px 宽（含1px间距）
4. **行高**：建议 12px 间距，避免文字重叠

## 网络配置

| 项目 | 值 |
|------|-----|
| WiFi SSID | DESKTOP-O6O2BC0 8856 |
| WiFi 密码 | 见被忽略的 wifi_secrets.h / 仓库外备份 |
| MQTT Broker | mqtt://8.163.110.27:1883 |

## 参考文档

- ESP-IDF 官方文档: https://docs.espressif.com/projects/esp-idf/
- VSCode ESP-IDF 扩展: https://docs.espressif.com/projects/vscode-esp-idf-extension/
- 项目进度: docs/PROGRESS.md
- 双项目切换: docs/双项目切换指南.md
