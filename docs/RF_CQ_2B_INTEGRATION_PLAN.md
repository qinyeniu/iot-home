# RF CQ-2b 集成方案：消费者直接持有发送事务

更新时间：2026-09-25
状态：设计文档；当前不改运行代码
前置阶段：CQ-1、CQ-2a 已默认关闭离线完成；07:25 干净 4 小时长测 DEGRADED

## 1. 为什么还需要 CQ-2b

CQ-2a 已让实验 ON 路径中的用户 QUERY / SET / AUTO 先进入 RF 队列，但消费者目前仍调用旧的：

- `rf_power_remote_query()`；
- `rf_power_remote_set_manual()`；
- `rf_power_remote_set_auto()`。

因此 CQ-2a 主要验证“生产者任务”和“执行任务”分离，还没有把以下动作全部收敛到消费者内部：

1. 检查 waiting transaction；
2. bump policy generation；
3. 预留 Zigbee packet seq；
4. 调用 ZBOSS APS API；
5. 根据发送结果安装或清除等待事务；
6. 更新 backoff。

CQ-2b 的目标是让消费者自己完成这些动作，关闭“序号已预留但尚未调用 ZBOSS API”这一最后竞态窗口。

## 2. 必须保留的产品语义

### 2.1 用户 QUERY

- 节点未激活：拒绝；
- 已有未超时等待事务：拒绝重复 QUERY；
- 旧事务已超时：用户 QUERY 可以接管；
- 发送前发布 `querying`；
- 成功发送后等待节点 REPORT；
- REPORT 必须匹配 packet seq，且 request id 属于本次用户事务。

### 2.2 用户 MANUAL SET

- 目标功率必须属于支持表：`-10, 0, 8, 14, 18, 20`；
- 用户意图先写入 RAM；
- 持久化 desired mode/manual power；
- bump policy generation；
- 节点不在线时，只保存策略，等后续 join/announce 后应用；
- 节点在线但当前功率未知时，当前实现先发 QUERY；CQ-2b 应保留这个行为；
- 节点在线且当前功率已知时，只发送一个 MANUAL SET。

### 2.3 用户 AUTO SET

- desired mode 写为 AUTO；
- 持久化当前 manual power，但不改变手动功率值；
- bump policy generation；
- 节点不在线或当前功率未知时，只保存策略；后续由周期补偿；
- 当前功率已知时，使用当前功率发送 AUTO SET；
- 占位功率不能作为真实目标功率发送。

### 2.4 全局边界

- 不新增 NVS key；
- 不改变分区表；
- 不改变前端/后端 API；
- 不写额外 Flash；
- 不部署服务器；
- 失败回退只需要烧录默认 OFF 网关应用区。

## 3. 目标调用结构

```text
MQTT 用户命令
    |
    |-- 校验参数
    |-- 更新 RAM desired policy
    |-- 持久化已有策略字段
    |-- 构造用户命令
    v
RF command queue
    v
rf-dispatch 单一消费者
    |
    |-- RF 状态锁：过期旧 waiting、检查当前状态、预留 seq
    |-- 释放 RF 状态锁
    |-- ZBOSS lock：调用 APS data request
    |-- 释放 ZBOSS lock
    |-- RF 状态锁：记录结果、安装 waiting 或清理失败状态
    v
等待 APS REPORT callback
```

禁止在持有 RF 状态锁时调用 ZBOSS lock 或任何 Zigbee API。

## 4. 实验 ON 时临时停掉旧周期路径

CQ-2b 还没有把周期/自动命令并入队列，但消费者必须成为实验固件中唯一的 RF 私有发送者。因此在实验 ON 构建中应临时让：

```c
rf_power_controller_periodic()
```

直接返回，不执行旧的周期 QUERY、reconcile 或自动功率 SET。

这不是最终产品形态：

- CQ-2b 阶段：旧周期路径在实验 ON 下临时禁用；
- CQ-3 阶段：周期/自动路径改为提交后台命令；
- CQ-3 完成后，消费者既处理用户命令，也处理后台命令。

必须在交接和提交信息中明确“实验 ON 期间不会自动周期补偿”，避免误判为功能缺失。

## 5. 建议拆分的内部函数

为避免一次性重写所有旧函数，建议先拆出小函数，所有函数仍只在实验 ON 条件下编译。

### 5.1 生产者准备函数

- `rf_power_prepare_user_query_locked()`；
- `rf_power_prepare_manual_policy_locked()`；
- `rf_power_prepare_auto_policy_locked()`。

职责只包括 RAM 状态、request id、policy generation 和 apply pending；不调用发送 API。

持久化仍在锁外执行，避免在临界区执行 NVS I/O。

### 5.2 消费者事务开始函数

建议新增：

```c
rf_cq_result_t rf_power_begin_dispatch_locked(
    const rf_dispatch_cmd_t *cmd,
    rf_power_dispatch_plan_t *plan
);
```

输出的 plan 至少包含：

- short address；
- packet seq；
- 实际 command；
- mode；
- power；
- 是否应该发布 querying；
- 是否因 waiting、inactive、unknown power 等原因不能发送。

该函数在 RF 状态锁内完成检查和 seq 预留。

### 5.3 APS 发送函数

保留独立的纯发送函数，例如：

```c
esp_err_t rf_power_execute_aps_request(
    const rf_power_dispatch_plan_t *plan
);
```

函数内部只做：

1. 检查 Zigbee stack started；
2. 获取 ZBOSS lock；
3. 构造 APS request；
4. 调用 `esp_zb_aps_data_request()`；
5. 释放 ZBOSS lock。

### 5.4 事务完成函数

发送返回后，在 RF 状态锁内执行：

- 成功：安装 waiting transaction；
- 失败：清除 expected user transaction；
- 无论成功或失败：调用现有 `rf_tx_note_result_locked()` 更新 backoff；
- 对 QUERY 的 `querying` 发布应在预留 seq 后、实际发送前或按现有语义保持一致。

## 6. TTL 与执行时刻

用户命令 TTL 当前为 5,000 ms。消费者开始执行时必须再次检查 TTL。

如果命令在队列中等待超过 TTL：

- 不应预留 seq；
- 不应调用 ZBOSS；
- 应输出可识别失败日志；
- 后续可扩展为发布 `failed` 状态。

CQ-2b 不要求后端立即消费 `failed` 状态，因为这会引入前后端语义变化；该内容单独设计。

## 7. 队列满与提交失败

当前队列容量为 8。队列满时不能阻塞 MQTT 任务。

CQ-2b 的最低要求：

- `rf_cq_submit()` 返回错误；
- MQTT 入口记录明确日志；
- 不发送 RF 帧；
- 后端可能等待到超时，这一限制必须记录。

完整 negative ACK 应作为独立阶段，不与 CQ-2b 混做。

## 8. 栈和资源

CQ-2a 将消费者栈调整为 6,144 字节，以适配旧函数。CQ-2b 应先保持该值，硬件短测后根据高水位决定是否下调。

判读阈值：

- 低于 512 字节：DEGRADED；
- 低于 256 字节：FAIL。

实验 ON 固件当前 bin：`0x1604e0`，应用分区余量 4%。若 CQ-2b 导致分区余量低于 3%，应先精简重复代码，不继续叠加功能。

## 9. 主机测试要求

CQ-2b 涉及 FreeRTOS 和 ZBOSS，不能完全靠主机测试，但纯逻辑仍需覆盖：

1. waiting 未超时时，不允许重复 QUERY；
2. waiting 超时后，用户 QUERY 可接管；
3. 节点 inactive 时，不预留 seq；
4. 不支持功率不进入 plan；
5. AUTO 计划必须重新读取当前功率，不能直接使用队列占位功率；
6. TTL 过期不进入 plan；
7. 一个成功 plan 只产生一个 seq。

如果某些函数必须放在目标编译条件下，应尽量把“决策结构”和“FreeRTOS/ZBOSS 执行”分离，便于 MinGW 编译。

## 10. 目标构建矩阵

每次改动必须验证：

```powershell
idf.py -DRF_COMMAND_QUEUE_ENABLED=ON build
idf.py -DRF_COMMAND_QUEUE_ENABLED=OFF build
```

并重新构建：

```text
firmware/gateway/tests/target_lifecycle
```

默认 OFF 必须继续满足：

- bin 回到 `0x15f540`；
- 不启动 rf-dispatch；
- 不创建 RF 队列；
- 旧周期路径恢复。

## 11. 硬件短测顺序

取得独立复审 Approve 后，按 `docs/RF_CQ_SHORT_TEST_RUNBOOK.md` 执行：

1. COM6 网关、COM5 节点；
2. 只烧录 COM6 网关应用区；
3. QUERY；
4. SET 0；
5. SET -10；
6. AUTO；
7. 正常拔掉节点，观察网关至少 60 秒；
8. 接回节点，确认 secure rejoin；
9. 使用 `tools/analyze_rf_cq_short_test.py` 判读。

若短测 DEGRADED 但业务仍可用，需要结合日志判断是否继续 CQ-3；若出现 FAIL，不进入下一阶段。

## 12. 独立复审门槛

CQ-2a 多次独立复审未返回有效结论，因此当前是“未取得独立 Approve”。

CQ-2b 开始改代码前必须重新取得有效独立复审。复审至少覆盖：

- RF 状态锁与 ZBOSS lock 的顺序；
- seq 预留和失败回滚；
- waiting transaction 生命周期；
- 周期路径禁用边界；
- AUTO 当前功率读取；
- TTL 和队列满；
- 默认 OFF 是否完全不变。

## 13. 回退方案

- 代码回退：回滚 CQ-2b 对应提交；
- 硬件回退：构建默认 OFF 并只烧录网关应用区；
- NVS：不新增 key、不改变格式；
- Zigbee NVRAM：不擦除；
- 云服务器：无变更；
- 前端/后端：无变更。

## 14. 结论

CQ-2b 的关键不是“把旧函数换个任务调用”，而是让一个消费者在同一条受控流程中完成决策、序号预留、发送和事务安装。配合实验 ON 下临时禁用旧周期路径，才能在进入 CQ-3 前真正封闭用户命令路径的竞态。
