# 远程开关设计（Zigbee 标准 On/Off 簇）

> 第一版主题“室内环境监测 + 远程开关”的后半部分。
> 本设计替代 2026-09-06 旧版 node_switch（Wi‑Fi 直连、绕过网关、硬编码密码）。
>
> **2026-09-22 更新：用户确认先不做第三块独立板，开关直接集成到原传感器节点。**
> 当前主线为 `firmware/node_zigbee`（传感器 + On/Off server cluster + GPIO4）。
> `firmware/node_switch_zigbee/` 保留为以后独立开关板的备选，不刷入当前硬件。

## 1. 端到端链路

```text
POST /api/devices/gw-001-zb-XXXX/commands
body: {"command": "on" | "off" | "toggle"}
        │
        ▼  (commands 表记录，已有)
MQTT  iot-home/gw-001/nodes/zb-XXXX/cmd
        payload: {"command":"on","payload":{},"ts":"..."}
        │
        ▼  网关解析：topic 末尾 node 短地址 + payload.command
Zigbee ZCL On/Off 簇 (0x0006) 单播
        命令字: Off=0x00  On=0x01  Toggle=0x02
        │
        ▼  节点 On/Off cluster server（与传感器同 EP10）
action: ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID (cluster 0x0006)
        │
        ▼
继电器 GPIO4（3.3V 低电平触发：拉低=接通，拉高=断开）
        │
        ▼  100 ms 后
ZCL Report Attributes：cluster 0x0006, attribute 0, Boolean(0x10)
        │
        ▼
网关转 MQTT telemetry：{"data":{"on_off":0|1}} -> metrics 表
```

服务器侧命令 API、commands 表、MQTT 发布已完成；本次只改固件。

## 2. 已实施改动

### a. 网关 firmware/gateway/main/main.c

MQTT_EVENT_DATA 中，当 topic 匹配 `.../nodes/{node}/cmd`：

1. 从 topic 提取 `{node}`：形如 `zb-XXXX` → 短地址 0xXXXX；
2. 解析 JSON 的 `command` 字段：on/off/toggle → 命令字 0x01/0x00/0x02；
3. 用 `esp_zb_zcl_on_off_cmd_req()` 单播到该短地址：
   - address_mode = `ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT`；
   - src/dst endpoint 均为 10；
4. 网关 EP10 添加 On/Off client cluster；
5. ZCL report callback 支持 OnOff Boolean report，并转换为 `on_off` 指标。

### b. 二合一节点 firmware/node_zigbee/main/main.c

原传感器节点继续承担 AHT20/BH1750 上报，同时在同一 EP10 添加：

- On/Off server cluster（0x0006）；
- GPIO4 输出，上电默认拉高（继电器 OFF）；
- core action handler：收到 SET_ATTR_VALUE 且 cluster=0x0006 时控制 GPIO4；
- 100 ms 后用 raw APSDE-DATA 发送一条 Boolean 类型的 ZCL Report Attributes；
- 继电器状态帧有独立在途记录，按 7 字节 Boolean ASDU 与 8 字节传感器帧区分，最多尝试 3 次，链路探测窗口占用时延迟重试。

节点仍是 Zigbee End Device，并设置 rx-on-when-idle，以便云端命令无需等待本地轮询即可到达。

### c. 独立开关板固件（备选，不是当前主线）

`firmware/node_switch_zigbee/` 保留独立 On/Off light 节点模板；当前用户决定不做第三块板，因此只保留代码和提交，不刷写、不扩展。

## 3. 验收结果（2026-09-22 00:06-00:12）

已完成软件/固件实测：

1. 节点 secure rejoin 成功，短地址保持 `0x82cb`，云端设备 `gw-001-zb-82cb`。
2. `on`：节点日志出现 `RELAY ON`，网关收到 cluster=0x0006 value=1。
3. `off`：节点释放，网关收到 `on_off=0`。
4. `toggle`：从 off 切到 on，网关收到 `on_off=1`。
5. 测试结束后再次 off，最终 `on_off=0`。
6. 温湿度/光照每 10 秒上报持续正常，完整数据包中包含 `on_off`。
7. 云端 metrics 表已能查询 `metric=on_off`。
8. 可靠性增强后复测：打开状态第一次尝试即 APS acknowledged；关闭后云端最新值恢复为 0，传感器上报持续正常。

待用户最终确认物理现象：继电器 IN/VCC/GND 是否已接好，是否听到“咔哒”声，或用万用表/小灯泡验证输出端通断。

云端发命令示例：

```text
curl.exe --noproxy "*" -s -X POST
  http://8.163.110.27:8000/api/devices/gw-001-zb-82cb/commands
  -H "Content-Type: application/json"
  -d "{\"command\":\"on\"}"
```

## 4. 注意事项：不要使用高层 OnOff report helper

在当前 ESP-ZBOSS 1.6.0 组合下，节点调用 `esp_zb_zcl_report_attr_cmd_req()` 会在命令执行后触发：

```text
Assertion failed .../zcl/zcl_general_commands.c:612
```

因此当前实现沿用传感器上报已经验证的 raw APSDE-DATA 路径：

- frame control：0x18；
- command：Report Attributes，0x0a；
- data type：Boolean，0x10；
- payload：1 字节 0/1。

后续升级 Zigbee SDK 后可以重新评估高层 helper，但没有实测前不要切回。

## 5. 暂不做（后续增强）

- 设备能力建模：当前设备是“传感器 + 开关”组合，后续不要简单改成纯 switch；可增加 capabilities 字段或依据 `on_off` 指标识别；
- 命令执行结果回执（commands 表 status: sent → done/acknowledged；当前 APS acknowledged 只是运行时链路确认）；
- Grafana 开关按钮/开关状态面板；
- 本地按键手动控制继电器；
- 更强状态一致性：若 3 次 relay report 全部失败，可在节点周期上报中携带 OnOff，或由云端超时后 read attribute。当前状态回传属于有有限重试的 best-effort。
