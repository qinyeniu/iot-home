# 远程开关设计（Zigbee 标准 On/Off 簇）

> 第一版主题"室内环境监测 + 远程开关"的后半部分。
> 本设计替代 2026-09-06 旧版 node_switch（Wi‑Fi 直连、绕过网关、硬编码密码）。

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
        ▼  节点 On/Off cluster server
action: ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID (cluster 0x0006)
        │
        ▼
继电器 GPIO（已购 3.3V 低电平触发：拉低=接通，拉高=断开）
```

服务器侧（命令 API、commands 表、MQTT 发布）**已完成**，本次不改。

## 2. 本次改动范围

### a. 网关 firmware/gateway/main/main.c
MQTT_EVENT_DATA 中，当 topic 匹配 `.../nodes/{node}/cmd`：
1. 从 topic 提取 `{node}`：形如 `zb-XXXX` → 短地址 0xXXXX；
2. 解析 JSON 的 `command` 字段：on/off/toggle → 命令字 0x01/0x00/0x02；
3. 用 `esp_zb_zcl_on_off_cmd_req()` 单播到该短地址，
   address_mode = `ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT`，
   src endpoint 10（EP_GW），dst endpoint 10。
命令通过 ZBOSS scheduler 上下文发送（与现有传感器 APS 同方式）。

### b. 新开关节点固件 firmware/node_switch_zigbee/
以 node_zigbee / 角色互换 join 固件为模板：
- Zigbee End Device（rx-on-when-idle，命令即时响应；继电器持续供电，低功耗无意义）；
- endpoint 10：`esp_zb_on_off_light_ep_create()`（On/Off cluster server 0x0006）；
- 注册 core action handler：SET_ATTR_VALUE 且 cluster==0x0006 时，
  读 OnOff 值 → 控制继电器 GPIO；
- BDB network steering 加入现有网络（复用已验证的 provisional/rejoin 处理）；
- 上电默认继电器 OFF（GPIO 高）；**不擦 NVRAM**（保持网络持久）；
- 不接 OLED/I2C，板载 WS2812（IO8）可选做开关指示灯。

## 3. 验收（硬件实测，本次核心）

1. 开关节点通电后自动入网，云端出现新设备（类型先按 sensor，下一步修正为 switch）。
2. 调用命令 API 发 on/off：
   - 清楚听到继电器"咔哒"吸合/释放声；
   - 串口日志显示收到命令、GPIO 翻转；
   - 可在继电器输出端接小灯泡/万用表验证通断。
3. 连续多次 on/off/toggle 稳定，无丢命令、无误触发。

云端发命令示例：

```text
curl.exe --noproxy "*" -s -X POST
  http://8.163.110.27:8000/api/devices/gw-001-zb-XXXX/commands
  -H "Content-Type: application/json"
  -d "{\"command\":\"on\"}"
```

## 4. 暂不做（验证后紧接着的下一步）

- 开关状态回传：节点 ZCL report OnOff 属性 → 网关转 MQTT → 云端/Grafana 显示当前开关态；
- 新设备正确注册为 type="switch"（当前后端遥测统一注册为 sensor）；
- 命令执行结果回执（commands 表 status: sent → done）；
- 本地按键手动控制继电器。
