# Zigbee 应用事件队列 / 事件处理拆分方案（设计先行）

- 编制日期：2026-09-28
- 状态：**设计草案，待 8 小时长测结论后实施**
- 关联基线：`867c082 恢复默认OFF八小时夜间长测采集`
- 目标文件：`firmware/gateway/main/main.c`（当前 5332 行）

## 1. 背景与目标

P0-2/P1-3/P1-4 已依次完成 RF-quiet rejoin、节点模型组件（`iot_zb_node_model`）、
Wi-Fi 恢复策略组件（`iot_wifi_recovery_policy`），`main.c` 仍有 5332 行。
本阶段聚焦其中的 **Zigbee 应用事件子系统**：把事件类型、队列投递、事件处理
逐层从 `main.c` 拆为可独立测试的组件，进一步缩小 `main.c`，同时**严格保持
现有行为与时序不变**。

硬性约束：

1. 不改变任何业务语义、队列容量、drop 计数口径和锁顺序；
2. Flash 应用区仅剩约 54 KB，每个子阶段后都要核对二进制大小与剩余空间；
3. 每个子阶段独立提交、独立硬件短测，出现异常立即回退，不叠加改动；
4. 长测未出结论前**不实施**，只保留本设计。

## 2. 当前事件子系统盘点

### 2.1 数据流

```text
ZBOSS 回调（可能处于受限/ISR-like 上下文）
  esp_zb_app_signal_handler (3128)
    ANNCE            -> JOIN 事件  -> s_zb_app_q (业务队列, 32)
    LEAVE_INDICATION -> LEAVE 事件 -> s_zb_app_q
  zb_action_handler (3342)
    ZCL report       -> REPORT 事件-> s_zb_app_q
  signal_handler 内 PERMIT_JOIN/NLME，及通用 signal
                   -> SIGNAL 事件 -> s_zb_signal_q (诊断队列, 16)
                            |
                     s_zb_queue_set（队列集）
                            v
  zb_forward_task (5043)
    业务事件 -> zb_handle_app_event (5029)
                  join  (4746) / leave (4786) / report (4830)
    诊断事件 -> zb_handle_signal_event (4901)
    每轮还执行: status_resync / zb_forward_pending (4960)
                rf_power_controller_periodic / mqtt_outbox_flush / drop 日志
```

### 2.2 代码清单（main.c 行号，2026-09-28 基线）

| 类别 | 函数 / 对象 | 行号 |
| --- | --- | ---: |
| 事件类型 | `zb_app_event_type_t` | 288 |
| 事件结构 | `zb_app_event_t` + 静态断言 | 295-307 |
| 队列常量/句柄 | `ZB_APP_QUEUE_LEN`、`ZB_SIGNAL_QUEUE_LEN`、两队列、队列集、drop 计数 | 309-321 |
| 投递核心 | `zb_queue_try_send`（区分 ISR/任务，FromISR/yield） | 325-358 |
| 投递封装 | `zb_app_event_send` / `zb_signal_event_send` / `zb_app_signal_event_send` | 360-389 |
| NLME ABI | packed 结构 + 静态断言 | 391-416 |
| NLME 限速 | `zb_nlme_diagnostic_allowed` | 420 |
| ZBOSS 总入口 | `esp_zb_app_signal_handler` | 3128-3260 |
| ZCL 回调 | `zb_action_handler` | 3342-3397 |
| 节点表 | `s_zb_nodes` / `s_zb_store` / `s_zb_lock` | 3277-3282 |
| join 处理 | `zb_handle_join_event` | 4746-4784 |
| leave 处理 | `zb_handle_leave_event` | 4786-4813 |
| report 处理 | `zb_handle_report_event` | 4830-4900 |
| signal 处理 | `zb_handle_signal_event` | 4901-4952 |
| telemetry | `zb_forward_pending` + dedup 状态 | 4954-5008 |
| 业务分发 | `zb_handle_app_event` | 5029-5041 |
| 消费任务 | `zb_forward_task` | 5043-5100 |
| 支撑函数 | `zb_persist_child_seen` / `zb_publish_gateway_status` / `zb_publish_node_status` / `zb_publish_known_statuses` / `zb_publish_node_switch_state` / `zb_have_full_active_sample` | 4672-5027 |
| 队列创建 | `app_main` 内队列集/两队列创建 | 5219-5233 |

### 2.3 关键耦合

事件处理器不操作硬件，但调用大量 `main.c` 内 static 能力：

- 节点模型组件：`iot_zb_node_*`（已组件化）+ `s_zb_lock`；
- RF 功率：`rf_power_reset_for_announce` / `rf_power_publish_querying` /
  `rf_power_forget_node`；
- MQTT：`mqtt_is_connected` / `mqtt_pub` / `mqtt_pub_retained`；
- NVS：child marker 持久化；
- 计数：`s_zigbee_devices`、`s_zb_report_events`、telemetry dedup 数组。

因此处理器组件必须通过**显式服务接口（依赖注入）**调用这些能力，
不能直接 `#include "main.c"` 或反向依赖应用层。

## 3. 拆分原则

1. 沿用现有两种组件形态：
   - 纯逻辑 → `firmware/common/<name>`，配主机测试（参考 `iot_wifi_recovery_policy`）；
   - 依赖 FreeRTOS/协议栈 → 同样位于 `common`，以 `idf_component_register` 注册；
2. 每层只依赖比自己更基础的层，禁止组件反向依赖 `main`；
3. 类型与枚举的数值、字段布局保持不变，避免 ABI/持久化影响；
4. 一次只拆一层，main.c 仅做 include 与调用点替换；
5. 每阶段交付：主机测试（适用时）→ 默认 OFF 构建 → 二进制大小核对
   → COM6 硬件短测 → 代码复审 → 显式提交。

## 4. 分阶段实施计划

### P2-1：事件类型与事件构造（纯逻辑组件）

新组件：`firmware/common/iot_zb_app_event/`

- `include/iot_zb_app_event.h`
  - `iot_zb_app_event_type_t`（JOIN=1 / REPORT / LEAVE / SIGNAL，数值不变）
  - `iot_zb_app_event_t`（字段与现状一致）
  - NLME packed ABI 结构与静态断言
- `iot_zb_app_event.c`（纯函数）
  - `iot_zb_app_event_make_join(addr, ieee)`
  - `iot_zb_app_event_make_leave(ieee)`
  - `iot_zb_app_event_make_report(addr, cluster, raw, value)`
  - `iot_zb_app_event_make_signal(signal, status, value)`
  - `iot_zb_nlme_parse(payload, out_status, out_addr, out_cmd)`：
    按字节读取 offset=1 的短地址，保持非对齐安全
- 主机测试：事件构造字段、NLME 解析、枚举值与结构体大小断言

main.c 改动：删除本地类型定义，改 include 与构造调用。
风险低；不涉及队列与任务。

### P2-2：事件队列投递层（FreeRTOS 薄封装）

新组件：`firmware/common/iot_zb_event_queue/`

- API 草案：

```c
typedef struct {
    QueueHandle_t app_q;
    QueueHandle_t signal_q;
    QueueSetHandle_t queue_set;
    volatile uint32_t app_drops_task, app_drops_isr;
    volatile uint32_t signal_drops_task, signal_drops_isr;
} iot_zb_event_queues_t;

bool iot_zb_event_queues_create(iot_zb_event_queues_t *q);
bool iot_zb_event_send_app(iot_zb_event_queues_t *q, const iot_zb_app_event_t *e);
bool iot_zb_event_send_signal(iot_zb_event_queues_t *q, const iot_zb_app_event_t *e);
QueueSetMemberHandle_t iot_zb_event_select(iot_zb_event_queues_t *q, uint32_t wait_ms);
bool iot_zb_event_receive_app(iot_zb_event_queues_t *q, iot_zb_app_event_t *e);
bool iot_zb_event_receive_signal(iot_zb_event_queues_t *q, iot_zb_app_event_t *e);
uint32_t iot_zb_event_total_drops(const iot_zb_event_queues_t *q);
```

- 保持 `xPortCanYield()` 判断、`xQueueSendFromISR` +
  `portYIELD_FROM_ISR`、零等待 `xQueueSend(...,0)` 等现有语义；
- 队列容量沿用 32 / 16，队列集容量为两者之和；
- 主机测试仅覆盖不依赖 FreeRTOS 的参数/容量逻辑；其余由硬件短测覆盖。

main.c 改动：`app_main` 用 create 替换 5219-5233 的手动创建；
各投递点改调用组件；drop 计数读取改走实例。

### P2-3：业务与诊断事件处理器（依赖注入，最大块）

新组件：`firmware/common/iot_zb_event_handler/`

- 定义显式服务接口，由 main.c 装配：

```c
typedef struct {
    bool  (*mqtt_connected)(void);
    void  (*mqtt_pub)(const char *suffix, const char *data);
    void  (*mqtt_pub_retained)(const char *suffix, const char *data, bool retain);
    void  (*rf_reset_for_announce)(uint16_t addr);
    void  (*rf_publish_querying)(uint16_t addr, const char *request_id);
    void  (*rf_forget_node)(uint16_t addr);
    void  (*persist_child_seen)(void);
} iot_zb_event_services_t;

typedef struct {
    const iot_zb_node_store_t *store;
    portMUX_TYPE *node_lock;
    const iot_zb_event_services_t *services;
    volatile int *device_count;
    uint32_t *report_events;
} iot_zb_event_env_t;

void iot_zb_event_dispatch_business(const iot_zb_event_env_t *env, const iot_zb_app_event_t *e);
void iot_zb_event_handle_signal(const iot_zb_event_env_t *env, const iot_zb_app_event_t *e);
```

- 搬迁 join/leave/report/signal 四个处理器，内部锁顺序保持
  “先 `s_zb_lock`，RF 相关在锁外调用”；
- 现状中 signal handler 的非 zero NLME 限速（`zb_nlme_diagnostic_allowed`）
  保持在投递前，不进入本组件；
- 本阶段体量最大、耦合最深，放在 P2-1/P2-2 之后，并允许再拆成
  P2-3a（business）与 P2-3b（signal）两次提交。

### P2-4：消费任务与 telemetry 整合（收尾）

- 将 `zb_forward_pending`、telemetry dedup 状态、drop 周期日志一并纳入
  P2-2/P2-3 组件或新薄组件 `iot_zb_forward`；
- `zb_forward_task` 搬出 main.c，任务名/栈深/优先级（`zb-fwd`/5120/5）
  保持不变；
- `esp_zb_app_signal_handler` 作为 ZBOSS 要求的全局符号仍留 main.c，
  但其内投递动作全部变为组件调用，函数显著变薄；
- 收尾核对：main.c 目标行数、无残留孤立 static 函数、无未使用 include。

## 5. 预期收益

- main.c 预计可减少约 600-750 行（事件定义/投递约 130 行、
  事件处理与转发约 430 行、队列创建与调用点替换若干）；
- 事件构造与 NLME 解析获得主机测试覆盖；
- 投递层与处理器可独立复审，ZBOSS 受限上下文的行为边界更清晰。

## 6. 验证与回归计划

每个子阶段：

1. 主机测试：新增组件 `100% tests passed`；
2. 默认 OFF 构建通过，记录 bin 大小 / 应用区剩余 / SHA256；
3. COM6 硬件短测（≥ 5 分钟），至少核对：
   RF sample、cached report、MQTT telemetry、duplicate suppressed、
   Resource snapshots、drop 计数、I2C/OLED、父链路/NLME；
4. 代码复审 APPROVE；
5. 显式 `git add` 本阶段文件并提交，不使用 `git add -A`。

任一步失败：停止后续阶段，定位异常时间与模块，必要时回退到上一提交；
不覆盖正在使用的固件基线。

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| ISR-like 上下文投递语义变化 | 队列丢事件/栈异常 | P2-2 原样保留 xPortCanYield/FromISR/yield，硬件短测重点覆盖 |
| 服务接口回调过多导致代码增大 | Flash 进一步紧张 | 分阶段核对大小；先搬纯逻辑；回调表只读静态分配 |
| 锁顺序被打乱 | 死锁 | 保持“zb_lock 先于 RF 锁、服务调用在锁外”，复审逐函数核对 |
| 长测已暴露 I2C clear bus 周期性事件 | 可能先转去排障 | 若长测 FAIL，本方案暂缓，优先定位 I2C，不覆盖固件 |

## 8. 实施前置条件

1. `backups/long-test-20260928-default-8h/analysis.md` 已生成；
2. 长测结论为 PASS（或 FAIL 项已完成排障并有新的稳定基线）；
3. 工作区无其他未提交的固件改动；
4. 用户确认后按 P2-1 → P2-2 → P2-3 → P2-4 顺序实施。
