# 网关固件资源占用分析

> 数据来源：`build/iot-home-gateway.map` (ESP-IDF v5.5.4, ESP32-C6, 2026-09-28 构建)

## 按功能域汇总

| 功能域 | Flash (KB) | Flash 占比 | RAM 静态 (KB) | RAM 占比 | 说明 |
|---|---:|---:|---:|---:|---|
| Wi-Fi/网络栈 | 552.3 | 39.0% | 80.5 | 40.7% | net80211/lwip/wpa/pp/phy/TCP-IP |
| Zigbee 协议栈 | 334.7 | 23.6% | 25.5 | 12.9% | ZBOSS + esp_zb_api + IEEE802154 PHY |
| 安全/TLS/MQTT | 170.0 | 12.0% | 0.7 | 0.3% | mbedTLS + MQTT + TLS 传输 |
| RTOS/系统基础 | 129.4 | 9.1% | 55.5 | 28.0% | FreeRTOS/newlib/libc/heap/系统 |
| 存储/NVS/OTA | 120.7 | 8.5% | 1.0 | 0.5% | NVS + OTA + app format |
| 驱动/HAL/外设 | 60.8 | 4.3% | 22.6 | 11.4% | 驱动/HAL/GPIO/USB/SPI flash |
| 应用业务 (main+iot_*) | 43.6 | 3.1% | 12.1 | 6.1% | 网关主逻辑 + 自有 IoT 组件 |
| 其他 | 4.7 | 0.3% | 0.0 | 0.0% | 未归类组件 |
| **合计** | **1416.2** | 100% | **197.8** | 100% | |

## Top 15 组件 Flash 占用

| 组件 | Flash (KB) | RAM (KB) |
|---|---:|---:|
| libzboss_stack.zczr.a | 300.6 | 13.15 |
| libnet80211.a | 180.2 | 18.58 |
| liblwip.a | 118.4 | 3.65 |
| libpp.a | 107.0 | 46.09 |
| libesp_app_format.a | 102.7 | 0.01 |
| libmbedcrypto.a | 97.3 | 0.43 |
| libwpa_supplicant.a | 76.9 | 1.31 |
| libesp_hw_support.a | 49.5 | 20.58 |
| libmbedtls.a | 33.2 | 0.23 |
| libphy.a | 30.1 | 5.42 |
| libmain.a | 26.0 | 12.02 |
| libesp_system.a | 17.9 | 6.83 |
| libmqtt.a | 17.7 | 0.00 |
| libnvs_flash.a | 16.8 | 0.02 |
| libc.a | 16.5 | 0.57 |

## 说明

- 应用分区 `factory` 大小 0x170000 = 1,507,328 B (1472 KB)，bin 大小 1,452,832 B (约 1418.8 KB)，剩余约 54 KB (3.6%)。
- ESP32-C6 内置 SRAM 512 KB，其中 DRAM 约 320 KB 可用；静态占用约 198 KB，运行时 free heap 约 180 KB。
- 百分比基于功能域合计（Flash 1416.2 KB 为 map 统计，与 bin 的差异来自 appdesc/头部）。