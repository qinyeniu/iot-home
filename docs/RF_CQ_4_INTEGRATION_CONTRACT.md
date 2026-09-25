# RF CQ-4 网关集成契约

## 1. 文档目的

CQ-1 到 CQ-3f 已完成默认关闭的 RF 单队列、用户命令派发、后台 QUERY/SET 纯策略，以及后台命令完成/超时/失败状态策略。本文定义这些模块如何安全接入 `firmware/gateway/main/main.c`。

本文件只确定集成边界和分阶段计划，**不在本文阶段启用实验开关、不修改前后端、不部署云服务**。

## 2. 当前代码事实

### 2.1 默认 OFF 路径

- `s_rf_power_lock` 是网关 RF 运行态唯一短临界区锁；
- `zb_forward_task()` 周期性调用 `rf_power_controller_periodic()`；
- 周期函数在锁内完成准入、快照、序号预留判断；
- `rf_power_send_to_node()` 在锁外获取 ZBOSS lock 并调用 `esp_zb_aps_data_request()`；
- 发送成功后回到 RF 锁内安装 waiting transaction；
- `rf_power_aps_indication_cb()` 只接受加密、活动子设备、序号/功率/模式匹配的 REPORT。

这条路径是当前现网运行路径。

### 2.2 实验 ON 路径

- `rf_cq_configure_dispatch(rf_power_dispatch_from_queue)` 安装队列消费者回调；
- 用户 QUERY/SET/AUTO 先构造成 `rf_dispatch_cmd_t`，再调用 `rf_cq_submit()`；
- `rf-dispatch` 任务出队后进入 `rf_power_dispatch_from_queue()`；
- 当前消费者只处理 `RF_CQ_SOURCE_USER`；
- 实验 ON 的 `rf_power_controller_periodic()` 目前只调用 `rf_power_reconcile_pending_intent()` 后返回。

因此，CQ-3 的后台 QUERY/SET 计划虽然已通过主机测试，但尚未在 `main.c` 中运行。

### 2.3 命令表边界

后端 MySQL `commands` 表支持 `pending/sent/acknowledged/failed/superseded/timeout`，但网关当前没有直接写后端命令表的 API。CQ-3f 的 `table_action` 是给未来集成层的动作描述，不等于当前固件可以直接写数据库。

CQ-4 不擅自增加网关到后端的写入接口；后台 timeout/failed 先体现在：

1. MQTT RF policy 状态（可被节流）；
2. 网关本地日志；
3. RAM 中活动命令的关闭。

后端命令生命周期 API 属于后续独立阶段，需要先确认接口、鉴权、服务器资源和前端展示，再实施。

## 3. 非目标

CQ-4 不做以下事情：

- 不把 `RF_COMMAND_QUEUE_ENABLED` 默认改为 ON；
- 不刷 bootloader、分区表，不擦 Flash 或 Zigbee NVRAM；
- 不修改 MQTT TLS/WSS/VPN；
- 不修改阿里云 ECS、安全组、MQTT Broker 或后端部署；
- 不修改前端页面；
- 不新增高频轮询、常驻任务或数据库写入；
- 不要求电池深睡节点立即执行后台 SET。

## 4. 状态所有权

### 4.1 已存在状态

- `s_rf_node_addr`：当前活动节点短地址；
- `s_rf_last_rx_ms`：最近收到节点有效帧的时间；
- `s_rf_node_power_dbm` / `s_rf_node_mode`：节点最近已知功率/模式；
- `s_rf_policy_generation`：策略代际；
- `s_rf_report_waiting`：是否正在等待节点 REPORT；
- `s_rf_wait_started_ms`：等待开始时间；
- `s_rf_power_confirmed_ms`：最近确认时间；
- `s_rf_tx_backoff_until_ms`：本地发送失败退避结束时间；
- `s_rf_mode` / `s_rf_manual_power_dbm`：网关保存的目标策略；
- `s_rf_policy_apply_pending`：节点仍需收敛到目标策略；
- `s_rf_user_intent*`：实验 ON 路径中的用户意图状态。

### 4.2 CQ-4 可增加的最小状态

仅在实验 ON 条件编译内增加：

```c
static bool s_rf_background_active;
static rf_dispatch_cmd_t s_rf_background_command;
```

用途：

- 标记当前是否存在已提交给单队列或已发送的后台命令；
- 保存命令 id、source、generation、QUERY/SET、目标模式/功率；
- 在 REPORT、timeout、send failure 回调中核对命令身份。

不新增任务，不新增定时器线程；后台评估继续复用 `zb_forward_task()` 的周期入口。

## 5. 锁和调用顺序

### 5.1 允许的顺序

后台评估必须遵循：

1. 获取 `s_rf_power_lock`；
2. 快照运行态并调用纯策略；
3. 对 IDLE/STALE/READY 等结果计算 next state；
4. 若 READY，在锁内写入后台 ownership 并复制命令；
5. 释放 RF 锁；
6. 调用 `rf_cq_submit()`；
7. 若提交失败，重新进入 RF 锁并调用 CQ-3e rollback。

消费者实际发送时遵循：

1. 获取 RF 锁；
2. 再次核对命令 id/source/generation；
3. 通过 `rf_dispatch_plan_create()` 预留低层 packet sequence；
4. 安装 submitting/waiting 快照；
5. 释放 RF 锁；
6. 获取 ZBOSS lock；
7. 调用 `esp_zb_aps_data_request()`；
8. 释放 ZBOSS lock；
9. 回到 RF 锁记录成功/失败并收敛状态。

### 5.2 禁止事项

- 不在 RF 临界区内调用 MQTT、NVS、ZBOSS send 或阻塞式队列操作；
- 不持有 ZBOSS lock 时等待 RF 锁，以避免锁顺序反转；
- 不在命令进入队列前预留 packet sequence；
- 不把已出队但发现用户意图已抢占的后台 SET 继续发送；
- 不仅凭功率相同确认命令，必须核对 source、generation 和活动事务。

## 6. 分阶段实施计划

### CQ-4a：后台命令 RAM 记录与映射（不提交 RF）

修改范围限定于实验 ON：

- 增加 `s_rf_background_active` 和 `s_rf_background_command`；
- 增加从主 RF 状态填充 `rf_background_runtime_state_t` 的内部映射函数；
- 主机测试覆盖字段映射、默认值和非法状态；
- 不改变 `rf_power_controller_periodic()` 的实际行为。

验收：

- CQ 全量主机测试通过；
- cppcheck 无告警；
- 默认 OFF 与实验 ON 固件均构建通过；
- 默认 OFF 行为不变。

### CQ-4b：后台命令提交到同一队列

在实验 ON 的周期入口中调用 CQ-3e runtime adapter：

- IDLE / POLICY_ALREADY_APPLIED：应用 next state；
- STALE：清理运行态 RF/waiting，保留节点地址和 durable desired policy；
- READY：记录后台 ownership，释放 RF 锁后提交命令；
- 队列提交失败：调用 CQ-3e rollback，只清本次后台 ownership，不清除 `s_rf_policy_apply_pending`。

消费者扩展：

- `RF_CQ_SOURCE_USER`：沿用现有用户意图发送路径；
- `RF_CQ_SOURCE_PERIODIC`：进入后台命令发送路径；
- 出队后若发现用户意图、waiting、dispatch active 或 generation 已变化，则不发送并内部关闭该后台命令。

后台 SET 因上述抢占没有发送时，`s_rf_policy_apply_pending` 保持 true，等待下一轮评估。

### CQ-4c：后台 REPORT 完成处理

在 `rf_power_aps_indication_cb()` 中识别活动后台命令：

- 匹配 QUERY REPORT：调用 CQ-3f，发布 `report_received`；
- 匹配 SET REPORT 且满足目标：发布 `confirmed`，关闭后台命令，清除 pending；
- mismatch report：no action，不关闭活动事务；
- 节点自发报告只有在满足活动后台 SET 目标时才可确认，否则不接管。

后台 periodic QUERY 的 `report_received` 不带用户 request_id；后台 SET confirmed 也不能冒用用户 request_id。后端等待者只按 request_id 完成用户操作，因此不会被后台命令误完成。

### CQ-4d：后台 timeout / send failure

等待超时：

- 周期入口检测 `s_rf_background_active`、waiting 和等待时长；
- 满 90 秒时调用 CQ-3f `WAIT_TIMEOUT`；
- 命令必定关闭，SET 保持 pending=true；
- 是否发布 timeout 由负向通知节流决定。

发送失败：

- `esp_zb_aps_data_request()` 返回非 OK；
- 调用 CQ-3f `SEND_FAILED`；
- 命令必定关闭，SET 保持 pending=true；
- 继续使用现有 `rf_tx_note_result_locked()` 维护发送退避。

### CQ-4e：硬件窗口短测

仅在用户明确允许打开串口和烧录网关应用区后执行：

1. 枚举确认 COM6 为网关；
2. 只烧录 COM6 应用区，不刷 bootloader/分区表；
3. 再接/确认 COM5 节点；
4. 验证 QUERY → SET 0 → SET -10 → AUTO；
5. 正常拔掉节点，确认网关不重启；
6. 检查后台 QUERY/SET 是否只产生预期命令，无重复发送和旧事务重放。

未完成硬件短测前，实验 ON 固件不得视为可产品化运行。

## 7. MQTT 负向通知节流

建议集成层维护：

```c
static uint32_t s_rf_last_negative_notice_ms;
#define RF_BG_NEGATIVE_NOTICE_MIN_MS 300000u
```

规则：

- timeout/failed 两条负向状态 5 分钟内最多发布一条；
- 即使不发布，活动命令仍正常关闭；
- 正向 `report_received` 和 `confirmed` 不受负向节流限制；
- 后台 QUERY 仍受 60 秒评估节奏和 freshness gate 约束。

该节流只在实验 ON 集成时生效，不改变 Broker 配置。

## 8. 资源预算

- 不新增任务；
- 队列容量仍为 8；
- RF 消费者栈先保持 6,144 字节；
- 不新增后端请求或数据库写入；
- 实验 ON 应用分区当前余量约 4%；若集成后余量低于 3%，必须先精简重复代码，不继续叠加功能。

## 9. 验证要求

每个小阶段必须执行：

1. 新增/更新主机测试；
2. CQ 全量普通模式与 `-DNDEBUG`；
3. 限定范围 cppcheck；
4. 默认 OFF 网关构建；
5. 实验 ON 网关构建；
6. 独立代码复审。

只有代码复审通过后才能提交 Git。硬件测试结果必须写入当天交接文档。

## 10. 留给用户判断的事项

以下内容不在 CQ-4 中自动决定：

- 是否把实验 RF 队列切换为默认 ON；
- 是否设计网关到后端的命令生命周期写入 API；
- 是否在前端展示后台 SET 的 timeout/failed；
- 是否启用 NVS encryption、Secure Boot、Flash encryption；
- MQTT TLS/WSS、VPN 和阿里云安全配置；
- 电池深睡节点 deferred command 的产品交互。

这些事项均需要单独设计和维护窗口，不应与 CQ-4 的固件接入混做。
