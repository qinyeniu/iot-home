# IoT-Home Gateway Firmware

ESP32-C6 网关固件，承担以下职责：

- Zigbee 3.0 coordinator；
- Wi‑Fi STA 接入家庭网络；
- MQTT 客户端和 Zigbee ↔ MQTT 桥接；
- 节点传感器数据缓存、聚合和去重；
- RF 功率策略、远程 QUERY/SET/AUTO 和发送失败后退避；
- OLED SSD1306 状态显示；
- 本地产品配置 NVS 读取。

> 当前事实来源：`docs/CURRENT_DEVELOPMENT_PLAN.md`、`docs/FIRMWARE_FULL_CHAIN_WALKTHROUGH.md` 和最新 `docs/HANDOFF_*.md`。

## 1. 当前功能状态

### 已在硬件上验证

- Zigbee coordinator、channel 26；
- 现网节点 secure join / authenticated secure rejoin；
- 温湿度、光照遥测从 Zigbee 到网关、MQTT、后端、MySQL 的上行链路；
- 标准 on/off/toggle 远程控制及 OnOff 状态回报；
- RF QUERY → SET 0 → SET -10 → AUTO 短序列；
- RF 发送失败后自动退避版本；
- 正常拔掉节点不会导致网关重启；
- OLED 状态显示。

### 仓库离线就绪但尚未全部烧录

- RF policy 相同值不重复写入 NVS，降低 Flash 磨损；
- On/Off 陈旧目标保护：节点超过 180 秒未存活时跳过开关命令；
- NVS A/B 完整配置 bundle 和启动优先级；
- RF 命令 selected node 目标匹配，避免多节点阶段误控；
- RF 远程命令与周期任务之间新增 policy generation guard：若周期快照在预留 Zigbee 序号前被用户命令插队，会跳过旧周期发送；
- SoftAP/HTTP 配网本身尚未接入。

这些能力只有在硬件维护窗口验证后才能视为上线。

## 2. 硬件和软件环境

- 芯片：ESP32-C6；
- 调试端口：实际端口以 Windows 设备枚举为准；历史测试中网关常为 COM6、节点常为 COM5，但不能默认固定；
- ESP-IDF：v5.5.4；
- 构建主机：Windows PowerShell。

注意：打开串口监视器或重新烧录可能触发布局复位；长测期间不要操作串口。

## 3. 编译

```powershell
cd C:\Users\HJB\Documents\iot-home\firmware\gateway
. C:\Espressif\v5.5.4\esp-idf\export.ps1
idf.py build
```

构建产物：

```text
build\iot-home-gateway.bin
build\iot-home-gateway.elf
```

烧录前必须重新计算并记录 SHA256：

```powershell
Get-FileHash -Algorithm SHA256 build\iot-home-gateway.bin
```

如果应用分区剩余低于 5%，不得继续叠加 SoftAP/HTTP 等大功能，应先精简页面/代码或重新规划分区。

## 4. 安全烧录原则

未经用户明确授权：

- 不刷 bootloader；
- 不刷分区表；
- 不整片擦 Flash；
- 不擦 Zigbee NVRAM；
- 不烧录节点。

维护窗口中若只更新网关应用区，先停止占用 COM6 的采集进程，再执行应用区写入：

```powershell
python -m esptool --chip esp32c6 --port COM6 --baud 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 4MB --flash_freq 80m `
  0x10000 build\iot-home-gateway.bin
```

实际 COM 端口和二进制路径必须按当时情况复核。

## 5. 凭据与配置

### 实验室编译期回退

本地开发可使用被 `.gitignore` 忽略的：

- `main/wifi_secrets.h`
- `main/mqtt_secrets.h`

模板分别为：

- `main/wifi_secrets.h.example`
- `main/mqtt_secrets.h.example`

不要读取、打印、提交或在文档中记录真实密码文件。

### 产品化 NVS

共享组件位于：

```text
firmware/common/iot_product_config/
```

当前支持：

- legacy `iotcfg` 散列键读取；
- `cfg_a` / `cfg_b` / `cfg_commit` A/B 完整配置；
- 启动时按 bundle → legacy → 编译期默认值尝试。

SoftAP/HTTP、配置测试连接、恢复出厂和错误回滚尚未接入。NVS encryption、Flash encryption、Secure Boot 涉及不可逆 eFuse，必须由用户明确确认后再做。

## 6. MQTT 主题约定

| 主题 | 方向 | 说明 |
|---|---|---|
| `iot-home/{gateway}/nodes/{node}/telemetry` | 网关发布 | 温湿度、光照、OnOff 状态 |
| `iot-home/{gateway}/nodes/{node}/status` | 网关发布 | retained 在线状态 |
| `iot-home/{gateway}/nodes/{node}/cmd` | 网关订阅 | on/off/toggle、RF 命令 |
| `iot-home/{gateway}/nodes/{node}/rf_policy` | 网关发布 | RF 策略/命令状态 |
| `iot-home/gateway/{gateway}/status` | 网关发布 | 网关 LWT/状态 |

短地址会变化，产品化阶段应使用 IEEE address 或稳定 node ID 做长期身份映射，短地址只用于当前通信。

## 7. 常见问题

### Wi‑Fi 连接失败

先看串口和 OLED 状态，不要切换电脑网卡或热点。可参考：

- `docs/GATEWAY_WIFI_RECOVERY.md`
- `docs/FIRMWARE_FULL_CHAIN_WALKTHROUGH.md`

### MQTT 登录失败

检查当前运行配置来自 bundle、legacy NVS 还是编译期默认；日志不得输出密码。

### RF 下行失败

单次失败不等同节点离线；若出现连续明确失败，分析器会按规则降级或 FAIL。不要在未确认根因时升级 ZBOSS、加入重启循环或擦 Zigbee NVRAM。

### 串口打不开

可能已有采集进程占用。先枚举和确认进程用途，不能为了释放端口直接结束未知进程。

## 8. 后续维护顺序

1. 完成烧录后 8 小时双串口长测；
2. 根据最终报告决定是否烧录当前离线候选；
3. 硬件窗口验证 stale On/Off 和节点拔插；
4. 验证 NVS A/B bundle 的 save/load/restart 和掉电恢复；
5. 再实施最小 SoftAP/HTTP；
6. 用户有空时教学式完成 MQTT ACL、TLS/WSS 或 VPN；
7. 稳定后再拆分 `main.c` 和推进电池节点硬件实测；
8. 后续将 RF 私有命令收敛到单一命令队列，彻底封闭“序号已预留但尚未提交”的最后竞态窗口。
