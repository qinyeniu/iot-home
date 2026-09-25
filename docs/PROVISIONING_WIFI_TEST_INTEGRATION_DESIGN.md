# SoftAP 配网 S2：临时 Wi‑Fi 测试集成设计

更新时间：2026-09-26 03:05
状态：设计修订版；不接入 `main.c`，不启动 SoftAP/HTTP，不写 NVS
上游：`docs/PROVISIONING_SESSION_DESIGN.md`、`docs/PROVISIONING_HTTP_ADAPTER_DESIGN.md`、`docs/SOFTAP_PROVISIONING_API_CONTRACT.md`

## 1. 阶段目标

S2 只验证候选家庭 Wi‑Fi 是否能完成关联和取得 IP 地址：

1. 用户通过 SoftAP 本地网页提交候选 Wi‑Fi/MQTT 参数；
2. 设备把候选配置保存在 RAM，不写 NVS；
3. 设备进入 provision owner，暂停普通 Wi‑Fi/MQTT 自动恢复；
4. 临时尝试连接候选 Wi‑Fi；
5. 成功收到 IP 后，会话进入 `wifi_passed_pending_mqtt`，继续停留在候选网络等待 S3；
6. 失败、取消或超时后恢复原 STA 配置和普通 owner；
7. SoftAP 全程保持，网页通过 `/status` 查看进度。

S2 不把 MQTT 连接结果作为测试结果，也不保存配置；MQTT 测试属于 S3，保存属于 S5。

## 2. 现有代码事实

当前网关 Wi‑Fi 路径位于 `firmware/gateway/main/main.c`：

- `wifi_start()` 创建默认 STA netif、初始化 Wi‑Fi、注册事件并启动 STA；
- `wifi_cb()` 处理 `STA_START`、`STA_DISCONNECTED` 和 `STA_GOT_IP`；
- `wifi_reconnect_task()` 包含重连、RF health scan、boot watchdog、周期性 `stop/start` 等恢复动作；
- 当前已调用 `esp_wifi_set_storage(WIFI_STORAGE_RAM)`；
- 当前模式是 `WIFI_MODE_STA`，S2 配网模式需要使用 `WIFI_MODE_APSTA`。

普通恢复逻辑中的以下动作都会影响候选测试：health scan、boot watchdog 的 `esp_restart()`、`esp_wifi_disconnect()`、`esp_wifi_connect()`、`esp_wifi_stop()`/`start()`，以及 `STA_START` 事件中的自动 connect。因此 owner gate 必须覆盖完整恢复路径，而不是只禁止一个 connect 调用。

## 3. 目标组件关系

```text
esp_http_server
    ↓ HTTP request
iot_provisioning_http
    ↓ statically enqueued command
provisioning task
    ↓ set candidate config / connect
Wi-Fi driver + event loop
    ↓ disconnect / got-ip event
provisioning event adapter
    ↓ serialized result event
provisioning task
    ↓ state transition
iot_provisioning_session
```

职责边界：

- HTTP 层：收包、返回 HTTP 响应，不调用阻塞式 Wi‑Fi API；
- HTTP 适配器：校验输入、限流、维护会话和 JSON 响应；
- provisioning task：命令队列、Wi‑Fi owner、模式切换、配置保存/恢复、连接和超时的唯一执行者；
- Wi‑Fi event adapter：把事件转换成带 test_id 的内部命令，不直接改会话；
- 会话内核：保存 queued/testing 状态、候选配置、测试结果和不可变快照。

## 4. Wi‑Fi owner 完整门控

定义 Wi‑Fi owner：

```text
normal      普通 STA/MQTT 自动恢复
provision  配网测试
```

进入 provision 时必须在同一把锁内完成：

1. 记录 owner generation 和 test_id；
2. 暂停/断开现有 MQTT client，防止它通过候选网络自动重连；
3. 冻结普通 Wi‑Fi 重连任务；
4. 保存当前 STA 配置和连接阶段；
5. 切换/保持 `WIFI_MODE_APSTA`；
6. 安装候选 STA config。

provision owner 下，普通恢复任务每轮循环最开始必须阻塞或等待，不执行：

- `wifi_rf_health_scan()` 或任何主动扫描；
- boot watchdog 与 `esp_restart()`；
- `esp_wifi_disconnect()`；
- `esp_wifi_connect()`；
- `esp_wifi_stop()`/`esp_wifi_start()`；
- `s_wifi_retry`、RF health 计数和环境扫描状态更新。

事件处理也必须检查 owner：

- `WIFI_EVENT_STA_START` 只有在 normal owner 下才能自动连接原配置；
- provision owner 下是否连接由 provisioning task 控制；
- candidate disconnect 可通知普通任务，但普通任务必须在 owner gate 处停止；
- 旧 generation 的 got-ip/disconnect 只能丢弃。

退出 provision owner 的唯一入口是：S3 后续完成、用户取消、测试失败/超时或致命错误。恢复旧配置后才能重新允许普通健康扫描和 boot watchdog。

## 5. 唯一命令队列模型

S2 采用“先排队，再 testing”的模型，不采用“先 testing 再尝试入队”的备选方案。

后续会话内核增加一个 queued 状态，建议命名：

```text
IOT_PROV_STATE_QUEUED
```

命令类型：

```text
START_TEST
CANCEL_TEST
WIFI_EVENT
TIMER_TICK
```

队列要求：

- 静态分配，建议深度 2；
- 队列项只保存命令类型、test_id 和 owner generation，不复制完整密码快照；
- 候选请求由会话在 queued 状态下持有；
- 所有 `/test`、`/cancel`、事件和超时都进入同一队列；
- 队列满返回 503；由于会话尚未排队，不能返回 202，也不能留下 active test；
- 若已有 queued/testing/test_passed 测试，POST `/test` 按现有规则返回 409；
- 队列项取消、复位或丢弃时清零 test_id；候选密码由会话取消逻辑清理。

状态转换：

```text
waiting → queued → testing → test_passed/test_failed → committed/error
```

POST `/test` 的 202 表示“已进入有界队列并持有候选配置”。provisioning task 处理 `START_TEST` 后才转 testing，并真正调用 Wi‑Fi driver。

## 6. 测试阶段位

provisioning task 维护 Wi‑Fi 连接阶段位：

```text
phase_starting
phase_scanning
phase_associating
phase_waiting_ip
phase_finished
```

驱动事件映射：

- 建立 provision owner 后、首次 candidate connect 前：`phase_starting`；
- 调用 candidate connect 并进入扫描/连接流程：`phase_scanning`；
- 收到关联进展或 driver 进入连接过程：`phase_associating`；
- 确认关联、等待 DHCP：`phase_waiting_ip`；
- got-ip：`phase_finished`。

ESP-IDF 的事件不一定对每个 AP 流程提供单独“已关联”通知；实施时必须以可获得的事件为准。若没有可靠关联确认，在 got-ip 前统一保持 `phase_associating`，不得误报 waiting_ip。

## 7. 成功流程和成功后的状态

```text
POST /test valid
  → enqueue START_TEST, state=queued, HTTP 202
  → provisioning task: normal → provision
  → pause MQTT, save original STA config
  → set candidate config, connect
  → got-ip
  → iot_prov_note_wifi_result(passed=true)
  → state=test_passed；tests.wifi=passed；tests.mqtt=pending
```

成功后进入明确的逻辑状态：

```text
wifi_passed_pending_mqtt:
  owner = provision
  active STA = candidate network
  original STA config retained in RAM
  normal reconnect/health scan/boot watchdog frozen
  existing normal MQTT client remains paused
```

此状态不能提前释放 owner；否则普通重连可能抢回旧网络。S3 MQTT 测试继续使用候选网络和候选 MQTT 配置。只有 S3 完成后进入保存/切换流程，或 cancel/timeout/failure 触发恢复。

## 8. 失败、超时与取消

### 8.1 失败分类

| 事件或原因 | 会话错误 | 说明 |
|---|---|---|
| `WIFI_REASON_NO_AP_FOUND` / `..._IN_*` | `WIFI_ASSOC_TIMEOUT` | 未找到候选 SSID 或扫描条件不匹配 |
| `WIFI_REASON_AUTH_FAIL` | `WIFI_AUTH_FAILED` | 认证失败 |
| `WIFI_REASON_HANDSHAKE_TIMEOUT` | `WIFI_AUTH_FAILED` | 握手超时，常与密码错误相关 |
| `WIFI_REASON_ASSOC_FAIL` / `CONNECTION_FAIL` | `WIFI_ASSOC_TIMEOUT` | 关联失败 |
| `WIFI_REASON_BEACON_TIMEOUT` | `WIFI_ASSOC_TIMEOUT` | 关联后 AP 丢失 |
| 关联成功等待 IP 超时 | `WIFI_NO_IP_ADDRESS` | DHCP/IP 配置失败 |
| 其他未知原因 | `INTERNAL_ERROR` | 固件日志可记录数值 reason 供诊断 |

未知 Wi‑Fi reason 可记录到固件日志；HTTP 响应只返回通用 `INTERNAL_ERROR` 和字段名，不返回候选 SSID、密码或其他原始请求内容。

### 8.2 超时

会话总有效期为 5 分钟；单次 Wi‑Fi 测试独立超时建议为 20 秒。

按阶段位分类：

- `phase_scanning` / `phase_associating` 超时：`WIFI_ASSOC_TIMEOUT`；
- `phase_waiting_ip` 超时：`WIFI_NO_IP_ADDRESS`；
- timeout 与 got-ip 同时发生时，以 test_id、owner generation 和终态 CAS 判定；已经确认 got-ip 后不能被迟到 timeout 覆盖。

超时后 provisioning task 停止候选连接、恢复原 STA 配置、恢复 MQTT/normal owner，并标记失败。

### 8.3 取消

POST `/cancel` 本身只把 `CANCEL_TEST` 入队，由 provisioning task 执行：

1. 标记取消 generation，防止迟到事件覆盖结果；
2. 若 queued，移除 START_TEST 并直接回 waiting；
3. 若 testing，停止候选连接；
4. 恢复原 STA config，并按原状态决定是否重连；
5. 恢复暂停的 MQTT client；
6. 清理会话候选配置和密码副本；
7. 返回 waiting。

取消不写 NVS，不删除原有配置。

## 9. 现有 MQTT client 策略

S2 推荐策略：进入 provision owner 后暂停或断开当前 MQTT client。

原因：临时切换到候选网络后，现有 MQTT client 可能自动通过候选网络连接原 broker，产生非预期连接和日志噪声。暂停后：

- S2 的 `/status` 中 MQTT 步骤保持 pending；
- 现有 client 的连接结果不得写入 provisioning session；
- S3 使用候选配置启动/恢复测试 client；
- cancel/failure 恢复旧网络后，再恢复普通 MQTT client。

如果未来实施发现暂停 MQTT client 风险过大，可以退而保留 client，但必须在实现前补充隔离方案并重新复审；默认设计不再采用这种方案。

## 10. 配置保存和恢复

测试期间配置只存在 RAM：

1. 会话 queued 请求；
2. IDF candidate `wifi_config_t`；
3. 恢复用 original `wifi_config_t`。

要求：

- 保持 `WIFI_STORAGE_RAM`；
- 开始测试前复制原 `wifi_config_t`，恢复时原样写回；
- 候选 SSID/密码不得通过日志打印；
- 测试结束后清零候选配置和栈上密码副本；
- got-ip 不触发保存；
- 不调用 legacy save 函数；
- 当前没有可恢复连接时，回到等待状态而不是重启设备。

## 11. 实施顺序

1. 扩展会话内核：增加 queued 状态及静态队列所需接口；
2. 主机测试覆盖 waiting→queued→testing、重复入队、取消、过期和失败；
3. 实现 provision owner 和完整 Wi‑Fi 恢复门控；
4. 增加实验条件下的 APSTA/provisioning runtime，默认不启动；
5. 离线构建并记录默认 OFF/实验 ON 大小；
6. 硬件窗口按第 12 节表格验收；
7. S2 通过后再设计/实现 S3 MQTT 测试。

## 12. 硬件验收表

| 用例 | AP 夹具 | 操作时间 | 预期 `/status` | 预期错误 | 恢复要求 | 日志禁忌 |
|---|---|---|---|---|---|---|
| 正确 Wi‑Fi | AP-A：目标 SSID + 正确密码 + DHCP | POST 后立即开始 | 20 秒内 wifi=passed, mqtt=pending | 无 | 保持 candidate，等待 S3 | 不打印密码 |
| 错误密码 | AP-B：同 SSID/独立 SSID + 不匹配密码 | POST 后立即开始 | wifi=failed | `WIFI_AUTH_FAILED` | 恢复原 STA | 无普通重连抢连 |
| 错误/不存在 SSID | 不启动对应 SSID | POST 后立即开始 | wifi=failed | `WIFI_ASSOC_TIMEOUT` | 恢复原 STA | 无 health scan/stop-start |
| 无 DHCP | AP-C：关联正常但关闭 DHCP | POST 后立即开始 | wifi=failed | `WIFI_NO_IP_ADDRESS` | 恢复原 STA | got-ip 不得误报 |
| 弱信号 | AP-A 衰减或隔墙；RSSI 以实测记录，不预设固定阈值 | POST 后立即开始 | 20 秒内成功或明确失败 | 成功无错误；失败按阶段分类 | 恢复或保持 candidate | 不允许 boot watchdog 重启 |
| queued 取消 | 无需特殊 AP | 入队后 1 秒内 cancel | waiting | 无 | 不调用候选连接或安全终止 | 无旧事件回写 |
| testing 取消 | AP-A | 开始后 2–3 秒 cancel | waiting | 无 | 恢复原 STA/MQTT | 无重复连接风暴 |
| 重复/旧事件 | AP-A | got-ip 后提交旧 test_id 事件 | 保持 test_passed | 无 | 不改变 owner | 旧事件不得重放 |
| 正常拔节点期间测试 | AP-A + Zigbee 节点正常拔掉 | Wi‑Fi 测试过程中拔节点 | Wi‑Fi 结果不受 Zigbee 断链影响 | 无额外错误 | 网关不重启 | 无负向消息风暴 |

每个用例都要记录：请求时间、响应时间、阶段位、got-ip/disconnect reason、原 STA 恢复结果。HTTP 202 后若 20 秒未进入终态，视为测试工具失败并重新收集日志。

## 13. 离线验收标准

- 普通主机测试、`-DNDEBUG`、analyzer、cppcheck 通过；
- 默认 OFF 固件大小不增加；
- 实验 ON 固件大小有构建记录并满足应用分区余量；
- 不新增 NVS key、分区表或 Flash 写入；
- 主机测试覆盖普通恢复任务在 provision owner 下不会 health scan、warm reboot、disconnect/connect/stop-start。

## 14. 不在本阶段执行

- 不保存候选 Wi‑Fi/MQTT 凭据；
- 不把现有 MQTT client 的自动连接结果作为 S3 结果；
- 不配置 TLS/WSS/VPN，不修改阿里云 ECS、安全组、DNS 或 MQTT Broker；
- 不刷 bootloader/分区表，不擦 Zigbee NVRAM；
- 不修改前端和后端生产代码；
- 不启用需要 eFuse 的 NVS encryption、Secure Boot 或 Flash encryption。
