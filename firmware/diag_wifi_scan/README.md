# ESP32-C6 Wi-Fi / RF 临时诊断工程

用途：在不启动 OLED、MQTT、Zigbee 的情况下，单独验证 ESP32-C6 的 2.4GHz Wi-Fi 收发能力。

## 功能

- APSTA 模式；
- STA 连续主动扫描周围 2.4GHz AP，并打印 SSID、BSSID、信道、RSSI、认证方式；
- AP 侧发出开放热点 `IOT_RF_TEST`（信道 6）；
- 用于区分：
  1. 正式网关固件逻辑问题；
  2. 路由器兼容问题；
  3. ESP32-C6 板端射频、天线、供电或 PHY 问题。

## 2026-09-10 实机现象

- 电脑可看到 `CU_0D84` 2.4GHz，信道 3，信号约 91%；
- 网关 COM6 在 Zigbee 启动前即报 `reason=201/36 rssi=-128`；
- 本纯 Wi-Fi 诊断固件连续 8 轮扫描结果均为 0；
- 擦除 NVS/PHY 区（偏移 `0x9000`，长度 `0x7000`）后重新初始化，扫描仍为 0；
- AP 驱动日志显示 `sta + softAP` 已启动，但电脑多次扫描暂未看到 `IOT_RF_TEST`；
- 尚未完成真实断电 10 秒再上电复测，因此暂不把根因最终定为硬件损坏。

## 构建和烧录

```powershell
. C:\Espressif\v5.5.4\esp-idf\export.ps1
cd C:\Users\HJB\Documents\iot-home\firmware\diag_wifi_scan
idf.py set-target esp32c6
idf.py build
idf.py -p COM6 app-flash
```

## 恢复正式网关

诊断时只写入应用区，未擦除 Zigbee 分区。恢复网关：

```powershell
. C:\Espressif\v5.5.4\esp-idf\export.ps1
cd C:\Users\HJB\Documents\iot-home\firmware\gateway
idf.py -p COM6 app-flash
```

注意：诊断过程中擦除过 NVS/PHY 区；恢复正式网关后会重新初始化这些区域。
