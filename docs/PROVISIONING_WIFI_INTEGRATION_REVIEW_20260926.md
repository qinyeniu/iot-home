# SoftAP 配网 Wi‑Fi 集成复核（S2）

更新时间：2026-09-26 10:15
状态：集成前设计复核；不修改运行路径，不启用 SoftAP/APSTA，不写 NVS
上游：`docs/PROVISIONING_WIFI_TEST_INTEGRATION_DESIGN.md`

## 1. 复核结论

S2 目前已经具备四块可主机测试的 RAM-only 能力：

| 组件 | 作用 |
|---|---|
| `iot_provisioning_session` | 会话状态、QUEUED/TESTING/PASSED/FAILED、候选配置 RAM 快照 |
| `iot_provisioning_command` | 静态有界 FIFO，串行化 START/CANCEL/WIFI_EVENT/TIMER_TICK |
| `iot_provisioning_wifi_owner` | 区分普通 Wi‑Fi 恢复与配网测试的无线电控制权 |
| `iot_provisioning_wifi_event` | GOT_IP/DISCONNECTED 及断开原因映射 |
| `iot_provisioning_wifi_phase` | 连接阶段和连接/DHCP 超时分类 |

这些组件尚未被 `main.c` 调用，因此当前现网固件行为不变。**下一步不应该一次性把 SoftAP、HTTP、APSTA、MQTT 暂停和 NVS 保存全部接入**，应继续按小阶段集成，每个阶段编译、复审、提交。

## 2. 当前代码中的关键冲突点

当前网关 Wi‑Fi 路径：

1. `wifi_reconnect_task()` 会执行：
   - `wifi_rf_health_scan()`；
   - boot watchdog / `esp_restart()`；
   - `esp_wifi_disconnect()`；
   - `esp_wifi_connect()`；
   - 周期性 `esp_wifi_stop()` / `esp_wifi_start()`。
2. `wifi_cb()` 在 `WIFI_EVENT_STA_START` 时无条件 `esp_wifi_connect()`。
3. `mqtt_start()` 启动现有 MQTT client；若切到候选网络且 broker 可达，它可能自动重连，影响 S2/S3 判定。
4. 现有命令队列组件不是线程安全组件，真实集成中必须由单一 provisioning task 消费，或在外部锁保护下使用。
5. 当前应用分区只剩约 68 KB（5%），默认实验 ON 剩约 57 KB（4%）；HTTP server 和 gzip 页面必须最小化接入。

因此，如果不先接入 owner gate，普通 Wi‑Fi 恢复任务会在候选网络测试期间扫描、断开、重连或重启网关，导致测试结果不可信。

## 3. 推荐任务和锁模型

### 3.1 单一任务模型

新增未来任务：

```text
provisioning-task
```

它是唯一执行以下动作的任务：

- 修改 owner；
- 启停 MQTT client；
- 切换 APSTA/STA；
- 安装/恢复 Wi‑Fi config；
- 启动/停止候选连接；
- 处理 WIFI_EVENT 和 TIMER_TICK；
- 调用 session 状态机。

HTTP handler 和 Wi‑Fi event callback 只负责校验输入并提交命令，不直接调用阻塞 Wi‑Fi API。

### 3.2 锁边界

未来应使用普通互斥锁保护配网命令对象和 owner 状态；不能在 `taskENTER_CRITICAL()` 中调用阻塞式 Wi‑Fi/MQTT API。

锁职责：

```text
s_prov_lock：
- 保护静态命令队列、owner 状态和短小状态字段；
- 只用于快速复制命令，不包裹阻塞连接。

provisioning-task：
- 持有命令后释放 s_prov_lock；
- 在锁外执行 Wi‑Fi/MQTT 阻塞调用；
- 调用 esp_zb_lock 时不得位于 portMUX 临界区内。
```

现有锁顺序需要保持：

- 不在 `s_rf_power_lock` 或 `s_zb_lock` 临界区内获取 `esp_zb_lock`；
- 不在任何 spinlock 临界区内执行 `esp_wifi_*()`、MQTT、日志或重启。

## 4. 成功路径的严格顺序

```text
POST /test 合法
  → session queue_test()
  → START_TEST 入队
  → provisioning-task 取出 START_TEST
  → owner.acquire(test_id, generation)
  → 停止/暂停现有 MQTT client
  → 保存原 Wi‑Fi config 和连接阶段
  → 切换/保持 WIFI_MODE_APSTA
  → 安装候选 STA config
  → esp_wifi_connect()
  → 收到 GOT_IP
  → session.note_wifi_result(passed)
  → state=test_passed，wifi=passed，mqtt=pending
  → owner 保持 PROVISION，不释放
```

成功后不能释放 owner，也不能恢复原 STA；S3 MQTT 测试仍需停留在候选网络。

## 5. 失败、超时和取消

### 5.1 DISCONNECTED

```text
DISCONNECTED(reason)
  → wifi_event adapter
  → 映射 AUTH_FAILED / ASSOC_TIMEOUT / INTERNAL
  → session.note_wifi_result(failed)
  → 停止候选连接
  → 恢复原 STA config
  → 恢复 MQTT client
  → owner.release(generation)
```

### 5.2 TIMER_TICK

- `SCANNING` / `ASSOCIATING` 超时：`WIFI_ASSOC_TIMEOUT`；
- `WAITING_IP` 超时：`WIFI_NO_IP_ADDRESS`；
- `STARTING` / `FINISHED` 超时属于无效状态；
- GOT_IP 与 timeout 竞争时，由 session 的 test_id/generation 和终态 CAS 决定，已成功的结果不能被迟到 timeout 覆盖。

### 5.3 CANCEL

- QUEUED 取消：worker 尚未 acquire owner，不切换 Wi‑Fi；session 回 waiting，命令槽清零；
- TESTING/PASSED 取消：由 provisioning-task 停止候选连接、恢复原 STA/MQTT、释放 owner；
- 取消全过程不写 NVS。

## 6. 需要设计修订的一点

ESP-IDF 实际存在 `WIFI_EVENT_STA_CONNECTED`，它可作为确认关联、进入 WAITING_IP 的可靠信号。现有命令模型只定义了 GOT_IP/DISCONNECTED。

建议后续小阶段修订 `iot_provisioning_command`：

```text
新增 IOT_PROV_WIFI_EVENT_CONNECTED
```

并在 event adapter 中把 `WIFI_EVENT_STA_CONNECTED` 映射为该命令。若不新增该命令，则只能在 GOT_IP 前保持 ASSOCIATING，无法准确区分关联失败与 DHCP 失败。

## 7. 建议的后续小阶段

| 阶段 | 内容 | 安全边界 |
|---|---|---|
| I1 | 增加 provisioning task 骨架和生命周期日志 | 不启动 AP、不改 Wi‑Fi |
| I2 | 扩展 CONNECTED 命令与主机测试 | 不接入 `main.c` 运行路径 |
| I3 | 在普通 Wi‑Fi reconnect/STA_START 前读取 owner gate | 默认仍为 NORMAL |
| I4 | 接入 provision owner acquire/release，但先不启用 SoftAP | MQTT/APSTA 暂不切换 |
| I5 | APSTA 与候选 Wi‑Fi connect 硬件短测 | 只 app-flash COM6，准备回退 |
| I6 | 测试失败/超时/取消恢复 | 热点和节点在场 |
| I7 | S3 MQTT 测试 | 不改 TLS/WSS/VPN |
| I8 | A/B Bundle 保存与重启验证 | 需要维护窗口 |

## 8. 明确不自动执行

- 不修改阿里云 ECS、MQTT Broker、DNS、安全组；
- 不启用 TLS/WSS/VPN，后续由用户在场时教学式配置；
- 不修改前端或后端生产代码，除非后续单独评审；
- 不刷 bootloader/分区表，不擦 Zigbee NVRAM；
- 不启用 NVS encryption、Secure Boot 或 Flash encryption；
- 不把 S2 的 `/commit` 与 NVS 写入提前混入当前阶段。
