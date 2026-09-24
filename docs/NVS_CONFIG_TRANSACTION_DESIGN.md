# NVS 配置事务与恢复设计

更新时间：2026-09-24  
状态：设计文档，供用户决策；**当前代码未切换到新存储模型**  
适用范围：ESP32-C6 网关的 Wi‑Fi/MQTT 产品配置；Zigbee NVRAM 不纳入本设计

## 1. 背景

现有配置组件把 Wi‑Fi 和 MQTT 字段保存为多个独立 NVS 字符串键：

- `wifi_ssid`
- `wifi_pass`
- `mqtt_uri`
- `mqtt_user`
- `mqtt_pass`
- `mqtt_cid`
- `version`

这种结构便于开发和单步调试，但不适合作为 SoftAP `/commit` 的最终写入模型。

已通过代码审查确认：不能把“同一个 NVS handle 中多次 `nvs_set_str()` 后调用一次 `nvs_commit()`”理解为多键事务。普通 NVS handle 下，部分 `nvs_set_str()` 可能已经把数据写入底层 Flash；如果后续键因空间不足、硬件错误或掉电失败，前面的字段可能已经改变，形成半套配置。

## 2. 设计目标

1. `/commit` 成功时，完整 Wi‑Fi/MQTT 配置全部可用；
2. `/commit` 失败、掉电或重启后，不出现“Wi‑Fi 已更新但 MQTT 仍旧/缺失”的混合状态；
3. 配网失败和反复测试不写 Flash；
4. 只有用户明确保存时写 Flash，减少磨损；
5. 保留 CRC、schema version 和后续迁移能力；
6. 不整片擦 Flash，不清除 Zigbee NVRAM；
7. 后续可与 NVS encryption 共存，但本阶段不启用任何 eFuse 安全特性。

## 3. 数据规模估算

当前最大字段容量：

| 内容 | 最大字节数（含 NUL） |
|---|---:|
| Wi‑Fi SSID | 33 |
| Wi‑Fi 密码 | 64 |
| MQTT URI | 161 |
| MQTT 用户名 | 65 |
| MQTT 密码 | 65 |
| MQTT Client ID | 65 |

合计约 453 字节，加上版本、CRC、字段长度后仍明显小于 NVS blob 常规限制。因此把完整配置收敛为一个 blob 是可行的，不需要修改分区表。

## 4. 方案 A：单一完整 blob

### 4.1 存储格式

只保留一个逻辑配置键，例如：

```text
cfg_v2
```

Blob 建议包含：

```text
Header
  magic
  schema_version
  payload_length
  header_crc / payload_crc
Fields
  wifi_ssid
  wifi_password
  mqtt_uri
  mqtt_username
  mqtt_password
  mqtt_client_id
```

字段应显式序列化长度和内容，不直接依赖 C struct 的内存布局，避免 padding、编译器版本和后续扩展造成兼容问题。

### 4.2 写入流程

1. 在 RAM 中校验所有字段；
2. 序列化完整 blob；
3. 计算 CRC；
4. 调用一次 `nvs_set_blob(cfg_v2, ...)`；
5. 加载时只接受 magic、版本、长度和 CRC 均正确的 blob。

### 4.3 优点

- 模型简单；
- 读取路径清晰；
- 只有一个逻辑配置项；
- 后续迁移和加密支持较容易；
- 代码量小于 staging 方案。

### 4.4 风险

单一 blob 的崩溃安全性依赖 NVS 对单个 item 更新的一致性。虽然 NVS 本身设计为掉电后可恢复，但这仍不如“先写 staging，再写 durable marker”的两阶段方案明确。

## 5. 方案 B：staging blob + durable marker（推荐产品化）

### 5.1 键设计

```text
cfg_stage
cfg_commit
```

`cfg_stage` 保存完整配置 blob；`cfg_commit` 保存小的提交标记，例如：

```text
schema_version
generation
payload_length
payload_crc
```

旧散列键或旧 `cfg_v2` 可作为迁移来源，不立即删除。

### 5.2 写入与提交流程

```text
RAM 校验与序列化
  ↓
写 cfg_stage（不改变当前活动配置）
  ↓
读回并校验 CRC
  ↓
写 cfg_commit marker
  ↓
新配置逻辑生效
```

关键点：**marker 是最后写入的。**

- 如果 `cfg_stage` 写入失败：没有 marker，当前活动配置不变；
- 如果写 `cfg_stage` 期间掉电：没有 marker，当前活动配置不变；
- 如果 marker 写入前掉电：stage 是临时数据，启动时忽略；
- marker 存在且 stage 校验通过：以 stage 为活动配置；
- marker 存在但 stage 损坏：不使用半套配置，进入配网/回退路径。

### 5.3 启动恢复

启动时按顺序判断：

1. `cfg_commit` 存在，且 `cfg_stage` 完整、CRC 正确：使用 staging 配置；
2. marker 不存在：使用旧的活动配置或 legacy 散列键；
3. marker 存在但 stage 不完整：不尝试拼接字段，进入安全回退/配网；
4. 不因为发现临时 staging 就立即执行额外 Flash 清理，避免启动时增加写入。

临时 stage 可在下一次用户明确保存时覆盖；是否立即清理应由实现阶段根据空间策略决定。

### 5.4 优点

- 失败和掉电边界最清楚；
- 当前活动配置不会被半成品覆盖；
- 适合 SoftAP `/commit`；
- 配网保存是低频操作，额外 marker 写入对 Flash 寿命影响很小。

### 5.5 成本

- 代码比单一 blob 更复杂；
- 需要定义 generation、marker、启动恢复和旧数据迁移；
- 需要硬件在线时模拟失败、取消和重启路径。

## 6. 方案 C：多字符串 staging + marker

也可以为每个字符串分别写 `staging_*` 键，再写 marker。但该方案存在：

- 键数量多；
- 写入路径长；
- 恢复逻辑复杂；
- 更容易出现字段缺失和版本不一致。

因此不推荐。既然完整配置小于 512 字节，应优先使用完整 blob。

## 7. 旧数据迁移

当前 namespace 中已有散列字符串键。建议迁移策略：

1. 新格式不存在时，允许读取旧散列键，保持现有实验室流程；
2. 不在启动时自动迁移，避免无用户感知的 Flash 写入；
3. 用户下一次通过 SoftAP 保存并测试成功时，写入新格式；
4. 旧键先保留一段版本周期作为回退；
5. 等新格式经硬件验证后，再在明确的维护版本中处理旧键，不与 SoftAP 首次接入混在同一次改动里。

## 8. 推荐决策

### 推荐给产品化：方案 B

理由：SoftAP `/commit` 的核心是“未完整提交就不改变活动配置”。staging blob + durable marker 能清楚表达这一点，也便于测试掉电、普通写入失败和取消路径。

### 如果希望先做最小可用版本：方案 A

单一完整 blob 可以作为中间步骤，但文档和代码中应明确它依赖 NVS 对单个 item 的更新一致性，不能宣称具备完整跨阶段事务恢复。

## 9. 实施前置条件

选择方案后再实施，且应拆分为独立提交：

1. 增加序列化/反序列化和 CRC，不改变运行路径；
2. 增加读取新格式能力，旧格式仅作回退；
3. 增加单一 blob 或 staging 写入接口；
4. 硬件在线时验证失败、取消、重启和掉电恢复；
5. 通过后才允许 SoftAP `/commit` 调用新接口；
6. 最后再考虑旧键迁移和清理。

## 10. 明确不做事项

- 不自动启用 NVS encryption；
- 不启用 Secure Boot 或 Flash encryption；
- 不修改分区表；
- 不清除 Zigbee NVRAM；
- 不在每次测试失败时写 Flash；
- 不把多个 `nvs_set_str()` 包装后称为事务；
- 不在用户未选择路线前直接替换现有配置存储。