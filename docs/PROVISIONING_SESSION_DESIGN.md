# SoftAP 配网会话内核设计

更新时间：2026-09-26  
状态：S0 离线会话内核；不接入网关运行时，不启动 SoftAP，不写 Flash  
上游契约：`docs/SOFTAP_PROVISIONING_API_CONTRACT.md`

## 1. 阶段目标

把 SoftAP 配网中最容易出错、且不依赖硬件网络的状态逻辑抽成独立 C 组件：

1. 同一时刻只允许一个配网测试；
2. 校验 Wi‑Fi、MQTT、网关 ID 和 TLS 开关；
3. 测试上下文和不可变配置快照只保存在 RAM；
4. 测试通过后 5 分钟内允许保存；
5. 保存动作只输出快照，由外部决定何时调用 A/B Bundle；
6. 对测试请求做每分钟最多 5 次的限流。

本阶段不实现 HTTP parser、SoftAP、captive portal，也不修改当前网关主流程。

## 2. 为什么先做会话内核

- 可在主机上完整编译和测试，不占用设备窗口；
- 不写 NVS，错误输入不会增加 Flash 磨损；
- 能锁定“旧测试不能提交、过期快照不能保存、失败配置不能写入”等核心安全边界；
- 后续接入 `esp_http_server` 时，HTTP 层只负责请求解析和响应格式，状态规则不混在网页任务中。

## 3. 状态流转

```text
waiting
  ├─ begin(valid request) → testing
  │   ├─ Wi‑Fi passed + MQTT passed → test_passed
  │   └─ either failed             → test_failed
  ├─ test_passed + prepare_commit → output immutable snapshot
  ├─ commit succeeded             → committed
  ├─ commit failed                → error
  └─ cancel / timeout             → waiting
```

`testing` 状态到期后转为 `test_failed`，错误码为 `TEST_EXPIRED`，已经通过的 Wi‑Fi/MQTT 子步骤保持 `passed`；`test_passed` 到期后回到 `waiting`，不保留旧快照。`committed/error` 状态下重复准备提交返回 `INVALID_STATE`。

## 4. 输入校验

组件复用现有主机可测的凭据校验：

- SSID：必填，最长 32 字节，拒绝控制字符，不裁剪首尾空格；
- 非开放网络密码：8–63 字节；
- 开放网络：密码必须为空；
- MQTT host：主机名或 IPv4，不允许 userinfo、路径、反斜杠、方括号等；
- MQTT port：1–65535；
- MQTT username/password：可空，最长 64 字节；
- gateway ID：空或 `^[A-Za-z0-9_-]{1,64}$`；
- TLS：当前只允许 `false`，否则返回 `TLS_NOT_CONFIGURED`。

组件根据 host/port 生成 `mqtt://<host>:<port>`，并通过 URI 校验器确认格式。生成 URI 最长 160 字节。

## 5. 保存边界

`prepare_commit()` 不写 NVS，只把 RAM 中已经通过测试的快照复制给调用方。推荐调用顺序：

```text
prepare_commit
  ↓
iot_product_config_save_bundle(snapshot)
  ↓
成功：commit_succeeded
失败：commit_failed
```

这样 NVS 保存失败时，会话不会伪装成已提交；调用方可重试或取消。真正的 Flash 写入仍只发生在显式保存阶段，失败测试和反复表单校验不产生 Flash 写入。

## 6. 线程与集成约定

S0 组件本身不依赖 FreeRTOS，便于主机测试。后续 HTTP 适配层必须从单一任务调用，或在外层加锁。组件不读取、不打印、不记录任何密码字段。

## 7. 后续阶段

- S1：增加 HTTP 适配层，提供 `/status`、`/cancel` 和只读页面；
- S2：接入临时 Wi‑Fi 测试和失败回退；
- S3：接入 MQTT 测试；
- S4：在硬件窗口验证 A/B Bundle；
- S5：最后让 `/commit` 调用 A/B Bundle 保存接口。

