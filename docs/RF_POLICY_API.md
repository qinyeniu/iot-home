# 射频发射功率策略 REST API

**更新时间**：2026-09-23
**状态**：已实现，单元测试与硬件实测通过

## 1. 概述

后端新增射频发射功率策略接口，外部系统（未来的 Web 界面、脚本、Grafana 外部操作等）无需直接连接 MQTT、无需烧录固件，即可查询和修改 Zigbee 节点发射功率。

严格事务语义与 `tools/rf_power_config.py` 一致：

1. 每次操作生成 16 字符一次性 `request_id`；
2. 网关先发布 `querying`，实际通过加密 Zigbee 链路与节点交互；
3. 只有节点加密 v2 REPORT 的序号、功率、模式均匹配本次操作，网关才发布 `status="confirmed"`；
4. 后端只确认与本次 `request_id` 匹配的 confirmed 报文；
5. `querying`、`report_received` 等中间状态不能确认操作。

## 2. 支持的功率档位

```text
-10、0、8、14、18、20 dBm
```

## 3. 接口

### 3.1 查询功率策略

```http
GET /api/devices/{device_id}/rf-policy
```

默认返回后端内存缓存（后端重启后缓存为空，返回 `status:"unknown"`）。

示例：

```bash
curl http://localhost:8000/api/devices/gw-001-zb-82cb/rf-policy
```

响应：

```json
{
  "device_id": "gw-001-zb-82cb",
  "mode": "auto",
  "power_dbm": -10,
  "status": "report_received",
  "request_id": null
}
```

实时查询（向网关下发一次 QUERY 并等待本次 confirmed）：

```http
GET /api/devices/gw-001-zb-82cb/rf-policy?fresh=true&timeout=40
```

### 3.2 设置固定功率（手动模式）

```http
POST /api/devices/{device_id}/rf-policy?timeout=40
Content-Type: application/json

{"mode":"manual","power_dbm":0}
```

成功响应：

```json
{
  "device_id": "gw-001-zb-82cb",
  "mode": "manual",
  "power_dbm": 0,
  "status": "confirmed",
  "request_id": "dd52f87877f023c2"
}
```

手动模式下，节点不会因入网或 APS 失败自动升高功率。

### 3.3 恢复自动模式

```http
POST /api/devices/{device_id}/rf-policy?timeout=40
Content-Type: application/json

{"mode":"auto"}
```

恢复自动时保持节点当前功率档位，仅切换模式；之后真实失败才允许自动升功率。

## 4. 参数与状态码

| 参数 | 说明 |
|---|---|
| `timeout` | 等待 confirmed 的秒数，默认 30，范围 1–120 |
| `fresh` | 仅 GET：`true` 时发起实时查询 |

| 状态码 | 含义 |
|---|---|
| 200 | 操作已被节点确认 |
| 404 | 设备不存在 |
| 422 | 请求参数非法（如手动模式缺少/非法 `power_dbm`） |
| 500 | MQTT 命令发送失败 |
| 504 | 等待时间内未收到与本次事务匹配的确认 |

## 5. 数据落库

`status="confirmed"` 的策略上报会写入指标表，供 Grafana 展示历史曲线：

- `tx_power_dbm`：节点当前发射功率；
- `rf_mode`：`0=auto`，`1=manual`。

`querying` 等中间状态不落库，避免污染历史。

## 6. 命令历史

通过 POST 发起的手动/自动修改会在命令表记录：

- `rf_set_power` / `rf_set_auto`；
- 状态流转：`pending -> sent -> acknowledged`；
- 超时未确认的命令保持 `sent`，由命令超时监控后续处理。

fresh QUERY 属于只读操作，不记录命令历史。

## 7. 2026-09-23 硬件实测

本地临时后端（独立客户端 ID `iot-home-backend-localdev`，SQLite）连接公网 broker：

1. GET 缓存：正常返回；
2. GET `fresh=true`：`auto/-10 dBm` confirmed；
3. POST 手动 `0 dBm`：`manual/0 dBm` confirmed；
4. POST 自动：`auto/0 dBm` confirmed；
5. POST 手动 `-10 dBm`：`manual/-10 dBm` confirmed；
6. POST 自动：`auto/-10 dBm` confirmed。

单元/回归测试：**68 passed，3 skipped**；其中新增 18 个 RF 策略测试。

## 8. 后续

- 在 Web 界面中暴露模式切换与档位按钮；
- Grafana 增加功率与模式面板；
- 服务器加固：关闭 MQTT 匿名访问并限制主题权限。
