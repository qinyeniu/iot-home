# SoftAP 配网 HTTP 适配器设计

更新时间：2026-09-26 02:30
状态：S1 离线 HTTP 适配器；不接入 `main.c`，不启动 `esp_http_server`，不写 NVS
上游：`docs/PROVISIONING_SESSION_DESIGN.md`、`docs/SOFTAP_PROVISIONING_API_CONTRACT.md`

## 1. 阶段目标

把未来 `esp_http_server` 中和硬件无关的工作收敛为独立组件：

1. 按 method/path 分发 `/status`、`/test`、`/commit`、`/cancel`；
2. 校验 `Content-Type`、请求体长度和 JSON；
3. 将 JSON 字段映射为配网会话请求；
4. 输出固定结构的 HTTP 状态码和 JSON 响应；
5. `/commit` 通过可替换回调保存快照，适配器本身不写 NVS。

本阶段不启动 SoftAP、不打开 socket、不切换 Wi‑Fi、不连接 MQTT。

## 2. 请求模型

集成层把 HTTP 请求转换为：

```text
method
path
content_type
body/body_len
now_ms
```

适配器只接受短路径：

- `GET /api/provision/status`
- `POST /api/provision/test`
- `POST /api/provision/commit`
- `POST /api/provision/cancel`

未知路径返回 404；路径存在但方法不匹配返回 405。JSON 请求体最大 4096 字节；有 JSON body 时必须使用 `application/json`。

## 3. 测试编号和保存回调

适配器不直接使用随机数或 NVS，由集成层注册：

- test_id 生成回调：产生形如 `tmp-<alphanumeric>` 的本次会话唯一编号；
- commit 回调：接收已通过测试的不可变快照，调用后续 A/B Bundle 保存接口。

所有 POST `/test` 在 HTTP 适配器最开始就消耗一次 60 秒/5 次限流额度；413、415、400 等畸形请求也不能绕过。commit 回调返回结构化结果；保存失败时会话进入 `error`，不能伪装为 `committed`。

JSON 解析完成并释放 cJSON 后，适配器立即清零保存请求体的 RAM scratch；提交函数栈上的临时请求和快照副本也在返回前 best-effort 清零。会话内核中的测试快照属于业务状态，会按取消、失败、过期或提交生命周期清理。

## 4. HTTP 状态码约定

| 场景 | 状态码 |
|---|---:|
| status/cancel 成功 | 200 |
| test 已接受 | 202 |
| JSON/字段错误 | 400 |
| 请求体过大 | 413 |
| Content-Type 错误 | 415 |
| 已有活动测试 | 409 |
| 请求过快 | 429 |
| test/commit 编号错误或状态不允许 | 404/409 |
| 保存失败或内部错误 | 500 |
| 未注册保存回调 | 501 |

所有 API 响应由集成层加 `Cache-Control: no-store`。响应和日志不包含任何密码。

## 5. JSON 响应

`/status` 包含：

```json
{
  "state": "waiting",
  "ap_ssid": "IoT-Home-3F9A",
  "firmware_version": "0.1.0",
  "can_commit": false,
  "test_expires_at": null,
  "tests": {"input": "idle", "wifi": "idle", "mqtt": "idle"},
  "errors": []
}
```

错误对象只包含 `code` 和 `field`，不回显用户输入。

## 6. commit handler 时间语义

当前主机模型中 commit handler 是同步调用：

1. handler 必须快速、有界、不可长时间占用 HTTP server worker；
2. 只有完整 bundle 已经持久化并通过底层写入确认后，handler 才能返回 `IOT_PROV_OK`；
3. handler 的执行时间必须明显短于测试 5 分钟 TTL；真实接入时应为 Flash 写入设置超时和失败路径；
4. 如果后续保存动作无法保证快速完成，应设计异步保存状态与查询接口，不能只在回调结束后改用新 tick。

## 7. 后续真实接入

未来 `esp_http_server` 适配层只做：

1. 从 HTTP request 取 body；
2. 调用本组件；
3. 设置状态码、no-store 和 JSON content type；
4. 在网络测试完成后调用会话内核的结果接口。

真实 Wi‑Fi/MQTT 测试仍按 S2/S3 分阶段接入；真实 `/commit` 必须等待 A/B Bundle 硬件验证。
