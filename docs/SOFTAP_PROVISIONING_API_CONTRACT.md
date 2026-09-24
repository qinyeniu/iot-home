# SoftAP 本地配网接口契约

更新时间：2026-09-24  
状态：接口契约与静态原型；**尚未接入网关固件**  
对应页面：`docs/provisioning/softap-prototype.html`

## 1. 目标

网关在缺少可用配置时开启短时 SoftAP，用户连接设备热点后，通过本地网页完成：

1. 填写家庭 Wi-Fi；
2. 填写 MQTT Broker；
3. 先测试连接；
4. 测试通过后再写入 NVS；
5. 写入后切换/重启进入正常运行。

核心原则：**先测后存，失败不写，减少 Flash 磨损；任何接口不回显密码。**

## 2. 当前边界

第一版只规划 HTTP/SoftAP 基本能力：

- 不启用 MQTT TLS；
- 不修改公网服务器；
- 不修改安全组、网卡、VPN；
- 不清除 Zigbee NVRAM；
- 不启用 NVS encryption、Secure Boot 或 Flash encryption；
- 不把接口直接接入尚未硬件验证的主流程。

请求中 `mqtt_tls: true` 在 TLS 配置完成前应返回 `TLS_NOT_CONFIGURED`，不能伪装成已支持。

## 3. 状态机

```text
waiting
  ├─ POST /test 有效请求 → testing
  │                      ├─ Wi-Fi/MQTT 通过 → test_passed
  │                      └─ 任一失败       → test_failed
  ├─ test_passed + POST /commit → committed
  └─ POST /cancel / 超时 / 重启 → waiting
```

测试上下文只保存在 RAM 中，建议有效期 5 分钟，并且保存提交时使用不可变的配置快照和 `test_id`。设备重启、上下文过期、用户取消或表单变化后必须重新测试。

## 4. 通用规则

- 服务只绑定 SoftAP 接口；
- 不提供网络转发、NAT 或互联网访问；
- 请求携带 JSON Body 时，仅接受 `Content-Type: application/json`；`/cancel` 允许空请求体；
- 请求体上限建议为 4096 字节；
- 不使用 Cookie，不返回 `Access-Control-Allow-Origin: *`；
- 配网接口响应应包含 `Cache-Control: no-store`，避免本地浏览器或代理缓存配网状态；
- 建议同一设备最多同时运行一个测试；
- 建议限制为每分钟最多 5 次测试请求，超出返回 `RATE_LIMITED`；
- 错误信息不得包含密码、Token 或用户提交的原始敏感值；
- 固件日志只记录状态和错误码，不打印 Wi‑Fi/MQTT 密码。

## 5. 接口定义

### 5.1 获取状态

```http
GET /api/provision/status
```

示例响应：

```json
{
  "state": "waiting",
  "ap_ssid": "IoT-Home-3F9A",
  "firmware_version": "prototype",
  "can_commit": false,
  "test_expires_at": null,
  "tests": {
    "input": "idle",
    "wifi": "idle",
    "mqtt": "idle"
  },
  "errors": []
}
```

字段说明：

| 字段 | 说明 |
|---|---|
| `state` | `waiting`、`testing`、`test_passed`、`test_failed`、`committed`、`error` |
| `ap_ssid` | 当前 SoftAP 名称，不属于敏感凭据 |
| `can_commit` | 只有测试通过且上下文未过期时为 `true` |
| `tests` | 输入校验、Wi‑Fi、MQTT 的 `idle/pending/passed/failed` 状态 |
| `errors` | 结构化错误；不得包含密码 |

状态接口只返回“是否已配置”或测试状态，不返回历史密码、当前 Wi‑Fi 密码或 MQTT 密码。

### 5.2 测试配置

```http
POST /api/provision/test
Content-Type: application/json
```

请求示例：

```json
{
  "wifi_ssid": "Home-WiFi",
  "wifi_password": "example-password",
  "open_network": false,
  "mqtt_host": "your-server.example.com",
  "mqtt_port": 1883,
  "mqtt_tls": false,
  "mqtt_username": "gateway",
  "mqtt_password": "example-mqtt-password",
  "gateway_id": ""
}
```

成功接收并开始测试：

```json
{
  "test_id": "tmp-01JABCDEFG",
  "state": "testing"
}
```

HTTP 状态码使用 `202 Accepted`。前端随后轮询 `GET /api/provision/status`。

该步骤：

- 不写入 NVS；
- 不修改原有已保存配置；
- 使用提交的临时参数尝试连接；
- 失败时保留原配置并返回错误状态。

### 5.3 保存已通过测试的配置

```http
POST /api/provision/commit
Content-Type: application/json
```

请求：

```json
{ "test_id": "tmp-01JABCDEFG" }
```

成功：

```json
{
  "state": "committed",
  "can_commit": false
}
```

设备应只在此时将 RAM 中已通过测试的配置快照写入 NVS。失败的测试、过期的 `test_id` 不得触发写入。

现有 `save_wifi()` 与 `save_mqtt()` 是两次独立提交，不能直接暴露给 HTTP `/commit`。也不能把“同一 NVS handle + 一次 `nvs_commit()`”当作多键事务：ESP-IDF 普通 NVS 中的 `nvs_set_str()` 可能已经持久化部分键，`commit()` 不负责多键回滚。固件实施前应改为单一完整 bundle blob，或 staging + durable marker/启动恢复；掉电或运行时错误导致不完整时必须丢弃半成品，避免只写入 Wi‑Fi 而 MQTT 缺失。

写入完成后，网页应提示用户重启或等待设备自动切换。是否自动重启需在实现阶段确认；建议第一版由用户点击确认，避免突然断连。

### 5.4 取消测试

```http
POST /api/provision/cancel
```

该接口无需请求体，也不要求 `Content-Type`。

响应：

```json
{ "state": "waiting" }
```

该操作只清理 RAM 测试上下文，不删除 NVS，不写 Flash。

## 6. 字段校验

| 字段 | 规则 |
|---|---|
| `wifi_ssid` | 必填；使用原始值，不裁剪首尾空格；最长 32 字节；拒绝控制字符 |
| `wifi_password` | 非开放网络 8–63 字节；开放网络必须为空 |
| `open_network` | 布尔值；产品化默认不推荐开放网络 |
| `mqtt_host` | 必填；普通主机名或 IPv4；不得包含空白、控制字符、路径、反斜杠、`?`、`#`、`@`、`[`、`]`、`:` |
| `mqtt_port` | 1–65535 的整数；明文原型默认 1883 |
| `mqtt_tls` | 第一阶段只允许 `false` |
| `mqtt_username` | 可空；最长 64 字节 |
| `mqtt_password` | 可空；最长 64 字节 |
| `gateway_id` | 可空；非空必须匹配 `^[A-Za-z0-9_-]{1,64}$`；空值表示自动生成 |

`gateway_id` 同时作为 MQTT Client ID 使用：非空值应保存到 NVS 键 `mqtt_cid`，并加载到 `iot_mqtt_config_t.client_id`；空值在运行时按 `NULL` 处理，由 ESP MQTT 自动生成。由于它会进入 MQTT Topic 和 ACL 身份，必须拒绝 `/`、`+`、`#`、反斜杠、空白和控制字符。

设备根据字段生成 URI：

```text
mqtt://<host>:<port>
```

生成后的 MQTT URI 不得超过 160 字节。后续启用 TLS 时使用：

```text
mqtts://<host>:<port>
```

IPv6 地址、CA 证书和每设备证书不在第一版范围内。

## 7. 错误码

| 错误码 | 含义 |
|---|---|
| `INVALID_JSON` | JSON 无法解析 |
| `PAYLOAD_TOO_LARGE` | 请求体超过限制 |
| `UNSUPPORTED_MEDIA_TYPE` | Content-Type 不正确 |
| `INVALID_FIELD` | 字段长度、格式或取值错误 |
| `WIFI_AUTH_FAILED` | Wi‑Fi 认证失败 |
| `WIFI_ASSOC_TIMEOUT` | Wi‑Fi 关联超时 |
| `WIFI_NO_IP_ADDRESS` | 已连接但未取得 IP |
| `DNS_FAILED` | Broker 域名解析失败 |
| `MQTT_CONNECT_FAILED` | MQTT 连接失败 |
| `TLS_NOT_CONFIGURED` | TLS 尚未配置 |
| `RATE_LIMITED` | 请求过于频繁 |
| `TEST_NOT_FOUND` | `test_id` 不存在 |
| `TEST_EXPIRED` | 测试上下文已过期 |
| `TEST_NOT_PASSED` | 未通过测试就请求保存 |
| `INTERNAL_ERROR` | 未归类内部错误 |

错误响应建议格式：

```json
{
  "state": "test_failed",
  "errors": [
    {
      "code": "WIFI_AUTH_FAILED",
      "field": "wifi_password",
      "message": "Wi-Fi 认证失败，请检查密码。"
    }
  ]
}
```

## 8. 与静态原型的对应关系

`softap-prototype.html` 已实现：

- 字节长度校验；
- 开放网络与密码模式；
- MQTT 主机、端口、可选用户名密码；
- TLS 控件禁用并提示后续配置；
- “测试连接”与“通过后保存”两阶段；
- 测试通过后修改字段会立即使旧测试结果失效；
- 取消按钮和清空重填会清理未完成的模拟定时器；
- 保存阶段使用不可变测试快照和模拟 `test_id`；
- 网关 ID 使用 Topic/ACL 安全白名单；
- 错误汇总和状态提示。

静态原型使用模拟逻辑，不会调用本机网络或写入设备。在 Wi‑Fi 名称或 Broker 地址中输入“失败”可查看失败路径。

## 9. 固件实施顺序

1. 在独立分支或独立组件中实现 HTTP 路由和 JSON 解析；
2. 先实现输入校验与 `/status`，不写 NVS；
3. 实现 `/test` 的 Wi‑Fi 临时连接；
4. 再加入 MQTT 连接测试；
5. 确定单一 bundle blob 或 staging marker 事务方案，不能直接串联两次独立 NVS commit，也不能依赖普通 `nvs_commit()` 做多键回滚；
6. 最后实现 `/commit` 调用具备恢复语义的持久化接口；
7. 硬件在线时验证失败路径、测试通过路径、重启后读取 NVS；
8. 通过后再决定是否加入 SoftAP 超时、本地 PIN 和恢复出厂入口。

不得在静态原型验收前直接替换网关主状态机，也不得在未确认组件版本和构建 SHA 时烧录固件。

## 10. 验收标准

- 错误密码不会写入 NVS；
- 反复测试失败不产生 Flash 写入；
- 只有测试通过后 `/commit` 才写入；
- 掉电或异常不能留下半套可用配置；
- 写入后重启可从 NVS 读取完整配置；
- NVS 缺失或损坏时仍可进入配网模式；
- 状态接口和页面不显示任何密码；
- 测试失败后原配置仍可回退；
- 不影响 Zigbee NVRAM、节点链路和现有远程开关功能。
