# RF 私有命令单队列设计

更新时间：2026-09-25 06:45
状态：设计文档；当前不改运行逻辑、不烧录
适用工程：`firmware/gateway/`、`firmware/node_zigbee/`

## 1. 背景

当前 RF 私有命令有两个发送来源：

1. MQTT 任务收到用户 QUERY / SET MANUAL / SET AUTO 后直接调用 `rf_power_send_to_node()`；
2. `zb-fwd` 任务周期性调用 `rf_power_controller_periodic()`，也可能直接调用 `rf_power_send_to_node()`。

2026-09-25 凌晨已观察到实际竞态：

```text
RF power -> ... manual seq=3: ESP_OK
RF power -> ... manual seq=4: ESP_OK
RF power: report accepted ... seq=4
RF power: reject duplicate/old report seq=3 last=4
```

后续又确认早前 SET 0 的旧周期事务 `seq=4` 延迟到达节点，使节点真实偏离为 `manual/0`。这说明问题不只是客户端等待错序号，旧周期事务可能被真实执行。

当前已用 policy generation guard 关闭“周期任务锁外节流”和“预留序号前被插队”的主要竞态；但仍有一个窄窗口：周期任务预留序号之后、实际调用 ZBOSS API 之前，用户命令可能插入。

## 2. 目标

1. 所有 RF 私有命令的 Zigbee 发送只能由一个消费者执行；
2. 远程用户命令、周期 reconcile、自动功率调整只提交意图；
3. 同一时间最多只有一个 RF 私有 Zigbee 命令被提交；
4. 用户命令优先级高于周期/自动命令，但不能绕过请求确认和等待事务；
5. backoff、generation、waiting transaction、request_id 的校验规则集中管理；
6. 不新增 NVS key，不改变分区表，不增加 Flash 写入；
7. 保持可回滚：若新版本异常，可回滚网关应用区。

## 3. 非目标

本阶段不处理：

- MQTT TLS/WSS、VPN、安全组；
- NVS encryption、Flash encryption、Secure Boot；
- SoftAP/HTTP 配网；
- 电池节点 deferred command；
- 标准 ZCL On/Off 命令队列化；
- 跨网关或多协调器场景。

标准 ZCL On/Off 后续可复用同一调度思想，但不应与 RF 私有命令队列混在同一改动中。

## 4. 当前调用路径

| 来源 | 当前任务 | 当前行为 | 风险 |
|---|---|---|---|
| MQTT `rf_query_power` | MQTT 任务 | 立即 QUERY | 与周期任务并发 |
| MQTT `rf_set_manual` | MQTT 任务 | 持久化策略并立即 SET | 与周期 reconcile 并发 |
| MQTT `rf_set_auto` | MQTT 任务 | 持久化策略并立即 SET | 与周期 reconcile 并发 |
| 周期评估 | `zb-fwd` | 根据状态 QUERY/SET/keep | 可能重放旧策略 |
| APS report callback | ZBOSS 调度上下文 | 更新节点状态和 waiting 事务 | 不能阻塞或直接发送 |
| 健康检查 | `zb-fwd` | 只判断重启条件 | 不参与普通发送 |

问题根因是“发送所有权”分散在多个任务中，generation 只能降低竞态概率，不能从结构上保证单一提交。

## 5. 目标架构

```text
MQTT 用户命令      周期 reconcile      自动功率评估
      |                 |                   |
      v                 v                   v
              RF command queue（有界队列）
                        |
                        v
               rf-dispatch 单一消费者
                        |
        +---------------+----------------+
        |                                |
        v                                v
 RF 状态锁内校验/预留 seq          ZBOSS lock 内发送
        |                                |
        +---------------+----------------+
                        v
             RF 状态锁内安装 waiting 事务
```

任何生产者都不能直接调用 Zigbee APS 发送 API。

## 6. 核心数据结构（建议）

```c
typedef enum {
    RF_CMD_SOURCE_USER = 0,
    RF_CMD_SOURCE_PERIODIC,
    RF_CMD_SOURCE_AUTO,
} rf_cmd_source_t;

typedef struct {
    uint32_t id;
    rf_cmd_source_t source;
    uint8_t command;                 // RF_CMD_QUERY / RF_CMD_SET
    uint8_t desired_mode;            // RF_MODE_AUTO / RF_MODE_MANUAL
    int8_t desired_power_dbm;
    uint32_t policy_generation;
    char request_id[17];
    uint32_t created_ms;
    uint32_t deadline_ms;
    bool ignore_backoff;             // 用户命令可尝试；周期命令必须退避
} rf_dispatch_cmd_t;
```

建议资源初始值：

- queue length：8；
- dispatch task stack：4096；
- task priority：5；
- 队列项以值复制，不引用 MQTT payload 指针。

这些资源属于 RAM，不改变 Flash 分区。实际合并前必须构建并检查：

- 网关应用分区剩余不得低于当前 5% 的安全余量太多；
- FreeRTOS heap 留有足够余量；
- 若任务栈或代码体积超出预期，应先精简或重新评估，不强行叠加 SoftAP。

## 7. 生产者规则

### 7.1 用户 QUERY

1. 校验 request_id；
2. 创建 `RF_CMD_SOURCE_USER` 命令；
3. `ignore_backoff = true`；
4. 入队；
5. 不直接发送，不提前发布 querying。

如果队列满，发布/记录可识别的失败状态；不能阻塞 MQTT 任务等待队列。

### 7.2 用户 SET MANUAL

1. 校验目标功率；
2. 更新 RAM desired mode/manual power；
3. 持久化 desired policy（保持当前“用户意图先保存”的产品语义）；
4. bump policy generation；
5. 创建用户命令并入队。

即使节点当前不存在，也允许保存策略；后续 join/announce 再触发应用。

### 7.3 用户 SET AUTO

1. 更新 RAM desired mode；
2. 持久化 desired policy；
3. bump policy generation；
4. 创建用户命令并入队。

### 7.4 周期/自动评估

周期任务只提交 `RF_CMD_SOURCE_PERIODIC` 或 `RF_CMD_SOURCE_AUTO` 意图，并携带快照 generation。

- backoff 未结束：周期/自动命令不能发送；
- generation 已变化：消费者在预留序号前丢弃该周期命令；
- 命令超过 deadline：消费者丢弃；
- 周期/自动命令不能覆盖尚未处理的用户命令。

## 8. 消费者流程

`rf-dispatch` 任务从队列取出一个命令后执行：

1. 检查命令是否过期；
2. 获取 RF 状态锁；
3. 对周期/自动命令重新校验 generation；
4. 对周期/自动命令检查 backoff；
5. 检查当前 waiting transaction 是否允许新命令；
6. 预留 Zigbee packet seq；
7. 释放 RF 状态锁；
8. 获取 ZBOSS lock；
9. 调用 `esp_zb_aps_data_request()`；
10. 释放 ZBOSS lock；
11. 再获取 RF 状态锁；
12. 成功则安装 waiting transaction，失败则记录 TX failure/backoff；
13. 释放 RF 状态锁。

关键性质：步骤 6 到步骤 10 期间，其他任务不能直接发送 RF 私有命令；用户命令只能入队，必须等当前消费者完成。这样可封闭“序号已预留但尚未提交”的最后竞态。

不得在持有 RF 状态锁时获取 ZBOSS lock 或调用 Zigbee API。

## 9. waiting transaction 规则

1. 当前已有未超时 waiting transaction 时，新周期命令必须等待；
2. 用户 QUERY 当前只允许接管已经超时的 waiting transaction；
3. 用户 SET 如需接管，必须等待旧事务达到超时条件，避免旧节点同时收到两个相互冲突的策略；
4. 发送成功但等待节点 report 超时，由后续周期 QUERY 恢复；
5. 节点主动 unsolicited report 仍按现有 128–255 序号规则接受；
6. accepted report 必须匹配 packet seq、mode、power 和 request_id。

## 10. 队列合并与优先级

### 10.1 不合并用户命令

来自不同 request_id 的用户命令不做覆盖。否则较早请求可能永远收不到 confirmed/timeout 状态。

### 10.2 可合并周期命令

如果队列中存在未处理的周期评估，可只保留最新评估意图，但不能覆盖用户命令。

### 10.3 用户命令优先

建议实现为两个队列或一个队列加优先级：

- 用户命令优先；
- 周期/自动命令在后；
- 已被用户命令改变 generation 的周期项在消费者中跳过。

第一版也可以使用单一 FIFO，因为 generation 会丢弃旧周期项；前提是队列长度有限且用户命令不会被周期项阻塞过久。

## 11. backoff 策略

- 周期/自动命令必须遵守现有 300/600/1800 秒退避；
- 用户命令保留当前可显式尝试的语义，即 `ignore_backoff=true`；
- 用户命令若继续失败，仍会递增失败计数并延长 backoff；
- 不在 backoff 期间自动重启网关；
- 不改变现有持久化 desired mode/manual power。

## 12. MQTT 状态发布

当前 `querying` 状态在发送前发布。目标架构建议改由消费者在真正预留序号后发布，避免命令已入队但未发送时状态误导。

需要区分：

```text
queued       已接收，等待消费者
querying     已预留序号并开始 Zigbee 查询
confirmed    收到节点匹配回报
timeout      等待超时
failed       队列满、参数非法或本地发送失败
```

完整 MQTT negative ACK 与前后端命令状态模型可在后续阶段做；本设计先保证固件发送路径单一。

## 13. 主机测试计划

建议将纯逻辑拆出并增加主机测试：

1. 用户 QUERY 入队后不会直接发送；
2. 用户 SET MANUAL 后只产生一个 Zigbee seq；
3. 用户 SET AUTO 后只产生一个 Zigbee seq；
4. 周期命令携带旧 generation 时被跳过；
5. backoff 中周期命令被跳过；
6. backoff 中用户命令仍可尝试，但失败会延长 backoff；
7. waiting transaction 未超时时周期命令不能抢占；
8. waiting transaction 超时后用户 QUERY 可接管；
9. 队列满时不阻塞 MQTT 任务；
10. 周期命令可合并，用户命令不可合并；
11. tick wrap 下时间差仍正确；
12. APS 发送失败不会清除 durable desired policy。

## 14. 硬件验证计划

维护窗口中只烧录网关应用区，不刷 bootloader/分区表，不擦 Zigbee NVRAM。验证顺序：

1. 确认 COM5/COM6；
2. 只烧录 COM6 网关应用区；
3. QUERY；
4. SET 0：确认只有一个 RF 私有下行 seq；
5. SET -10：确认只有一个 seq；
6. AUTO：确认最终 `auto/-10 dBm`；
7. 在周期边界附近重复 QUERY/SET，验证用户命令不会与周期命令重复发送；
8. 正常拔掉节点，确认网关不重启；
9. 接回节点，确认 secure rejoin；
10. 再启动 4–8 小时长测。

通过标准：每个用户策略只有一个有效 RF 私有 Zigbee 帧；无旧周期事务重放；节点正常拔插不会造成网关重启。

## 15. 分阶段落地建议

1. **阶段 CQ-1**：增加队列数据结构、消费者任务和主机纯逻辑测试，但暂时让旧路径保留，通过配置不启用；
2. **阶段 CQ-2**：远程用户命令改为入队，验证 QUERY/SET；
3. **阶段 CQ-3**：周期/自动命令改为入队，并删除旧直接发送路径；
4. **阶段 CQ-4**：硬件窗口只烧录网关应用区，执行短序列和拔插测试；
5. **阶段 CQ-5**：4–8 小时长测，通过后再推进 SoftAP/凭据产品化。

每一阶段必须独立提交。CQ-3 删除旧路径前，不得同时做 SoftAP、NVS A/B 运行时接入或前后端状态改造。

## 16. 回退方案

- 代码回退：回滚对应提交；
- 硬件回退：只烧录上一版网关应用区；
- NVS：本设计不新增 key、不改变数据格式；
- Zigbee NVRAM：不擦除；
- 云服务器：无变更。

## 17. 结论

单队列不是单纯“加锁”，而是把 RF 私有命令的发送所有权收敛到一个消费者。这样才能从结构上消除远程用户任务与 `zb-fwd` 周期任务的重复提交。

该改造涉及任务模型和等待事务，属于中等规模固件重构；建议按 CQ-1 到 CQ-5 小步实施，不在疲劳或无硬件窗口时一次性切换。
