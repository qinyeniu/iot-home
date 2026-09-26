# IoT-Home 当前开发计划

更新时间：2026-09-26 11:52（Asia/Shanghai；I4a 完成 RF health scan 恢复静默接入，4 小时长测运行中）
事实来源：`docs/HANDOFF_2026-09-26.md`、`docs/HANDOFF/_2026-09-26.md`、`docs/HANDOFF_2026-09-25.md`

> 旧的 `docs/PROJECT_SUMMARY.md`、`docs/PROGRESS.md`、`docs/TODO_TOMORROW.md` 只作历史记录；新工作以本文件和最新交接为准。

## 1. 当前真实进度

系统是三层架构：Zigbee 节点 → ESP32-C6 网关 → Wi‑Fi/MQTT → FastAPI/MySQL/Grafana。

> 10:08 状态：4 小时双端长测运行中；S2 配网命令队列、owner gate、event adapter 和 phase/timeout 已完成离线验证。因电脑热点未广播，用户 QUERY/SET 序列等待网络恢复后继续。

### 1.1 已经验证的主链路

1. **Zigbee 链路**
   - 网关为 coordinator，节点为 end device；
   - channel `26`，PANID `0xd989`；
   - 当前节点短地址 `0x82cb`；
   - 节点复位后 authenticated secure rejoin 已验证。

2. **传感器与远程开关**
   - 现网单节点集成 AHT20、BH1750、继电器和本地按键；
   - on/off/toggle 远程控制已打通；
   - on_off 状态可随遥测/状态回报。

3. **RF 功率策略**
   - 00:36 旧短序列中 AUTO 当次确认超时，fresh QUERY 确认真实状态为 `auto/-10 dBm`；
   - 03:33 已只烧录 RF 发送失败后退避网关，03:36 新短序列 QUERY → SET 0 → SET -10 → AUTO 全部匹配确认；
   - 06:38 已只烧录 generation guard 网关应用区；06:40 短序列 QUERY → SET 0 → SET -10 → AUTO 全部匹配确认，fresh QUERY 同样为 `auto/-10 dBm`；
   - 07:25 干净 4 小时长测已完成，结论 DEGRADED：业务连续，但 RF 37 次中 4 次 `ESP_ERR_NO_MEM`；
   - 后续 4–8 小时长测应在 CQ-2/CQ-3 硬件短测后再安排；
   - 后端 RF policy REST API 和 Grafana 功率/模式面板已完成，但公网后端仍未部署这部分 API；
   - 01:17 CQ-4d 已接入后台 timeout/send failure；硬件短测仍待 CQ-4e。

4. **事务一致性**
   - 网关命令响应与节点主动 REPORT 序号边界已分离；
   - confirmed 必须匹配 request_id、模式、功率和 deadline；
   - 链路陈旧只终止本次请求，不清除 durable desired mode/manual power。

5. **网关健康与并发**
   - 非 Zigbee callback 中的 APS/ZCL 请求已通过 ZBOSS lock 串行化；
   - 健康重启只在“长期无有效子节点帧 + 持续 MAC no-buffer 风暴 + 事件数量/持续时间达标”时触发；
   - 2026-09-25 已再次实测：正常拔掉节点不会导致网关重启。

### 1.2 夜间新增的离线能力

1. **双日志长测分析器**
   - `tools/analyze_long_test.py` 可同时读取节点/网关日志，输出 `PASS / DEGRADED / FAIL / INCOMPLETE`；
   - 可识别网关 RF 下发明确失败、结果未知及连续失败；
   - 定向测试：`21 passed`，独立代码复审 **Approve**。

2. **NVS A/B 完整配置接口**
   - 新增 `cfg_a`、`cfg_b`、`cfg_commit` 三个 NVS blob；
   - 保存时先写非活动数据槽并提交，再切换 durable marker；
   - 带 schema、长度和 CRC32 校验；
   - 当前 `main.c` 尚未调用，运行时行为不变。

3. **电池传感器阶段 3**
   - 独立 `firmware/node_battery_sensor` 工程已支持 60 秒采集 AHT20/BH1750 和标准 ZCL 上报；
   - Zigbee 入网后按需初始化 I2C，采样和 APS 请求提交后释放 I2C；
   - 已离线构建和复审，未烧录。

4. **SoftAP 配网资产**
   - 静态页面：`docs/provisioning/softap-prototype.html`；
   - 接口契约：`docs/SOFTAP_PROVISIONING_API_CONTRACT.md`；
   - 页面 gzip 后约 5.7 KB；网关应用分区剩余约 67 KB，只能按最小 HTTP/SoftAP 分阶段接入；
   - 02:05 已新增 RAM-only 配网会话内核：单测试、5 分钟过期、输入校验、快照与限流；
   - 02:40 已新增离线 HTTP 适配器：四个接口路由、JSON 映射、状态码、全请求限流、敏感 scratch 清理；当前仍未启动 SoftAP/HTTP。
   - 03:23 会话内核已新增 QUEUED 状态和 queue/start 接口，TTL 从入队开始；真实 provisioning task、APSTA 和 Wi‑Fi 连接尚未接入。
   - 09:44 已新增静态深度 2 的 iot_provisioning_command 命令队列，覆盖 START/CANCEL/WIFI_EVENT/TIMER_TICK、stale generation 和槽位清理；真实 provisioning task 尚未接入。

5. **RF policy Flash 去重**
   - 相同 `mode/mpwr` 已存在时跳过 `nvs_set/nvs_commit`；
   - 缺键、读取错误或值变化时保守写入；
   - 离线构建与复审通过，**尚未烧录**。

6. **RF 远程/周期事务 generation guard**
   - 周期节流检查、占用和策略版本快照在同一把 RF 状态锁内完成；
   - 用户 QUERY/SET/AUTO 会更新 policy generation；
   - 周期快照若在预留 Zigbee 序号前被用户命令 overtaken，会跳过旧发送，不再占用第二个有效序号；
   - 网关 bin `0x15f540`，SHA256 `D748CE3C635F9E2A7AFCD05BB9FCF4047AE3DCC6A200FCD2059C037D1320DFBB`，复审 **Approve**；06:38 已只烧录网关应用区，短序列 **PASS**。

7. **RF 单队列 CQ-2a（实验路径，默认 OFF）**
   - 用户 QUERY / MANUAL SET / AUTO SET 在实验 ON 时通过有界队列提交；
   - `rf-dispatch` 单一消费者检查来源和 TTL 后复用现有成熟发送函数；
   - 周期 QUERY、reconcile、自动功率 SET 尚未入队，完整发送所有权收敛仍需 CQ-2b/CQ-3；
   - 已提供 submit/dequeue、栈高水位日志、短测判读脚本和硬件短测手册；
   - 实验 ON bin `0x1604e0`（4% 分区余量），默认 OFF bin `0x15f540`（5% 分区余量）；
   - 独立复审最终 **Approve**；跨 tick deadline Low 已修复，High/Medium/Low 均为 0。

8. **节点诊断 NVS 低磨损保存**
   - `rfctl/txp2` 已保存相同值时跳过 Flash 写入；
   - 保存与功率变更共用互斥锁，当前功率只快照一次；
   - NVS 失败保留 dirty，安静 60 秒超时后低频补试；
   - 节点 bin `0xdb190`，SHA256 `7349CF157F59B336B0D14D7F44624B1903179B9923F688A455BB6610D22C853C`，复审 **Approve**，尚未烧录。

9. **On/Off 陈旧目标保护**
   - RF QUERY/SET/AUTO 只校验 active child；
   - on/off/toggle 额外要求 180 秒内有 join/announce 或有效 ZCL report；
   - 不写 NVS、不改 active 状态；离线构建通过，**尚未烧录**；
   - MQTT negative ACK 和能力校验仍待前后端联动设计。

10. **RF 私有命令队列 CQ-1（默认关闭）**
   - 新增 `rf_command_queue.c/.h` 与主机测试，封装 FIFO、意图合并、generation/backoff/waiting/deadline 判断；
   - FreeRTOS 生命周期使用 Queue Set、私有 stop/done 信号、消费者自删除和静态 state mutex，已关闭 start/stop/submit 竞态；
   - 新增独立目标测试工程 `firmware/gateway/tests/target_lifecycle/`，离线构建 PASS；真实硬件运行留待后续硬件窗口；
   - 默认 `RF_COMMAND_QUEUE_ENABLED=OFF`，`main.c` 未调用，设备运行路径不变；
   - 普通模式与 `-DNDEBUG` 主机测试均 PASS；网关临时 ON 构建通过后已恢复 OFF，复审 **Approve**。
11. **凭据输入校验阶段 1a（未接入运行路径）**
    - 新增主机可测试的长度、NUL、ASCII C0/DEL 控制字符和 MQTT UTF-8 校验；
    - 通用文本 API 使用显式容量，避免未终止输入造成越界读取；
    - Wi‑Fi SSID/密码按 octet string 处理，MQTT 字段拒绝非法 UTF-8、surrogate 和 Unicode C1 控制字符；
    - 普通模式与 -DNDEBUG 主机测试均 PASS；独立复审 **Approve**；
    - 当前不接入 main.c、不改变 NVS 格式，后续 SoftAP 请求解析层应先复用该校验。

12. **MQTT Broker URI 格式校验阶段 1b（未接入运行路径）**
    - 新增 `mqtt/mqtts/ws/wss`、IPv4/DNS host、端口、WebSocket 路径的离线格式校验；
    - 拒绝 URI userinfo、fragment、非法端口、非法 percent-encoding 和不支持的 IPv6 literal；
    - `mqtts/wss` 仅作语法预留，不自动配置 TLS/WSS/VPN；
    - 普通与 `-DNDEBUG` 主机测试 PASS；网关默认 OFF 构建 PASS，bin 仍 `0x15f540`；
    - 独立复审 **Approve**，最终 High/Medium/Low 均为 0。

13. **Flash 写入节流策略阶段 B1（未接入运行路径）**
    - 新增批量条数、数据年龄、最小写入间隔、小时/每日预算的纯 C 策略；
    - 强制模式只能跳过批量/年龄/间隔，不能突破写入预算；
    - 支持 32 位毫秒计数回绕；普通与 `-DNDEBUG` 主机测试 PASS；
    - 未写 Flash、未改分区表、未改变 RAM outbox；独立复审 **Approve**。

14. **电池低功耗路线与唤醒调度（未接入运行路径）**
    - 新增电池低功耗路线，先实测电流、对齐 sample/poll、门控外设，再评估 deep sleep；
    - 新增纯 C 唤醒策略，输出入网、poll、采样、合并唤醒和本地事件；
    - 普通与 `-DNDEBUG` 主机测试 PASS，覆盖过期 deadline、tick 回绕和本地事件合并；不改变现网节点/前后端 API；
    - 独立复审最终 **Approve**，High/Medium/Low 均为 0。

15. **CQ-2b 纯发送计划（实验 ON，尚未接入）**
    - 新增消费者发送计划：QUERY waiting/接管、MANUAL 未知先查、AUTO 使用当前功率；
    - MANUAL/AUTO 可清理旧 waiting；计划模块不调用 FreeRTOS、ZBOSS 或 NVS；
    - 主机普通/`-DNDEBUG` 测试 PASS；实验 ON、默认 OFF、target_lifecycle 构建 PASS；
    - 独立复审 **Approve**，High/Medium/Low 均为 0。

8. **CQ-3/CQ-4b 后台 RF 策略与队列集成（实验路径，默认 OFF）**
   - 已完成后台准入、QUERY、SET、组合执行计划、网关运行态 adapter 和完成/超时/失败状态策略；
   - CQ 全量普通与 `-DNDEBUG` 主机测试 10/10 PASS，独立复审 Approve；
   - 新增 `docs/RF_CQ_4_INTEGRATION_CONTRACT.md`，明确锁顺序、命令身份、MQTT 节流、资源预算和硬件短测边界；
   - CQ-4b 已在实验 ON 下接入 `main.c`：后台 QUERY/SET 提交同一 RF 队列，提交失败可回滚 ownership；默认 OFF 现网路径不变。

## 2. 当前主要风险
### P0（历史证据）：烧录前 2 小时节点出现网关下行持续失败

01:01 已启动双串口被动长测。02:02 的 1 小时节点为 `DEGRADED`；03:02 的 2 小时快照已变为 `FAIL`：

- 节点/网关无重启；
- 无 assert/panic；
- 无观察缺口；
- 无 MAC no-buffer；
- 节点遥测持续到达，2 小时内 MQTT 遥测发布 606 次；
- 节点累计新增 7 次 APS 失败；
- 网关 RF QUERY 下发在 uptime 9412 秒后连续 47 次返回 `ESP_ERR_NO_MEM`。

这说明“节点 → 网关 → MQTT”的上行仍可用，但“网关 → 节点”的主动 APS 下发在该时段持续不可用。该 FAIL 是烧录 backoff 版本前的历史证据；后续短测与 07:25 的干净 4 小时观察均未再出现连续 47 次失败，但 4 小时结论仍为 DEGRADED，不能判定 APS 资源问题已根治。

### P0：网关主动下行 APS 资源异常

`ESP_ERR_NO_MEM` 是 `esp_zb_aps_data_request()` 返回的本地资源/缓冲错误，不是节点返回的错误，也不是已观测到的 `ZB_ESP_MAC: No buffer` 日志。可能与 ZBOSS 1.6.0 长时间重复加密 APS 事务后的内部缓冲释放、父/子链路状态或内部资源碎片有关。当前不做猜测性升级、重启策略或协议栈版本替换；这些需要用户在场的硬件维护窗口验证。

### P0：公网 MQTT 匿名/弱权限风险

公网 MQTT 曾观察到匿名连接。ACL 配置虽已写好，但未部署前风险仍在。部署必须选择用户明确允许的维护窗口，避免影响游戏或当前网络。

### P1：应用分区余量紧张

默认 OFF 网关应用分区仅剩约 5%（约 `0x10ac0` 字节）；实验 ON 队列固件约剩 4%（`0xdeb0` 字节）。SoftAP/HTTP、页面和校验逻辑继续叠加前必须先构建验收，不能直接加入完整 HTTP 配网功能。

### P1：NVS A/B 接口尚未硬件验证

A/B bundle 与启动路径已离线编译，但尚未验证真实硬件上的 save → restart → load、写入失败、异常 marker、掉电恢复和 legacy 回退。当前不得直接烧录或加入 SoftAP/HTTP。

### P1：RF 后台命令等待 CQ-4e 硬件短测

当前 generation guard 已在 COM6 运行，06:40 短序列未见重复帧或旧事务重放；正常物理拔节点也已通过。CQ-4b 已让实验 ON 的周期后台 QUERY/SET 进入同一队列，CQ-4c 已完成匹配 REPORT 的 confirmed/report_received、pending 清理和节点自发上报确认，CQ-4d 已完成 timeout/send failure、SET pending 保持和负向 MQTT 5 分钟节流。

下一步只剩 CQ-4e：等待硬件窗口，只烧 COM6 应用区短测。每个阶段均需主机测试、静态检查、离线构建和独立复审。

### P1：仓库工作区较乱

当前仍有大量未跟踪串口日志、临时脚本、固件备份和实验工程。它们包含实测证据，后续应先分类再归档或删除，不做批量清理。

## 3. 下一阶段建议顺序

### 阶段 A：4 小时长测已结束，结论 DEGRADED

1. COM6 已运行 generation guard；06:40 短序列和 fresh QUERY 结论 **PASS**；
2. 正常物理拔节点已复测：网关不重启，仅出现预期断链提示；
3. 07:25:08 启动的干净 4 小时双串口观察覆盖 14,400.3 秒；
4. 最终结论 **DEGRADED**：无重启、panic、观察缺口或业务黑障，但网关 RF 37 次中 4 次返回 `ESP_ERR_NO_MEM`；
5. 单队列主要解决发送所有权和并发重复提交，不承诺必然修复 ZBOSS 内部 APS 资源问题；
6. 后续硬件窗口重点观察失败前后 ZBOSS signal、APS repair、rejoin 与周期 QUERY 的关系；
7. 节点 NVS 低磨损固件暂不烧录，等待明确维护窗口。

### 阶段 A2：CQ-4d 已提交，硬件窗口等待 CQ-4e

1. CQ-4b 已在实验 ON 下让后台 QUERY/SET 复用同一 RF 队列；
2. CQ-4c 已接入后台 REPORT 的 CQ-3f 完成策略，不冒用用户 request_id；
3. CQ-4d 已处理 timeout/send failure、SET pending 保持和负向 MQTT 5 分钟节流；
4. 默认 OFF 现网路径不变；默认 bin `0x15f540`，实验 ON bin `0x162150`；
5. CQ-4e 已只烧录 COM6 网关应用区，节点已 secure rejoin，后台 QUERY seq=1/2 成功；
6. 用户 QUERY → SET 0 → SET -10 → AUTO 等待电脑热点恢复后继续；
7. 完整短测手册仍见 `docs/RF_CQ_SHORT_TEST_RUNBOOK.md` 和 `docs/RF_CQ_4_INTEGRATION_CONTRACT.md`；
8. 硬件窗口只烧录 COM6 应用区；不与 SoftAP、NVS A/B 运行时接入、TLS/VPN 或前后端改动混做。

### 阶段 A3：CQ-5/S1 已完成，真实接入仍需等待

1. S0 新增独立组件 `firmware/common/iot_provisioning_session`，只保存在 RAM，负责单测试上下文、字段校验、5 分钟过期、通过后快照和请求限流；
2. S1 新增独立组件 `firmware/common/iot_provisioning_http`，支持 `/status`、`/test`、`/commit`、`/cancel` 的路由、JSON 映射和状态响应；
3. 所有 POST /test 均在协议解析前限流，畸形 JSON、415、413、空 body 和字段错误都不能绕过；
4. Content-Type 接受 `application/json` 和 `application/json; charset=utf-8`；通用错误 body 使用 `state=error`，字段校验使用 `state=test_failed`；
5. commit handler 未注册时返回 501；handler 失败时会话进入 `error`，不会伪装 committed；响应和日志不回显密码；
6. 组件非线程安全，真实 HTTP 集成必须单任务调用或加锁；
7. 默认 OFF bin 仍为 `0x15f540`，实验 ON bin 仍为 `0x162150`；
8. S2 设计已完成并复审 Approve（C/H/M/L=0）：见 `docs/PROVISIONING_WIFI_TEST_INTEGRATION_DESIGN.md` 和 `docs/PROVISIONING_WIFI_INTEGRATION_REVIEW_20260926.md`；会话 QUEUED 状态、静态命令队列、Wi‑Fi owner gate、event adapter 和 phase/timeout 均离线完成；下一步按 I1–I8 小阶段集成；真实 `/commit` 必须等待 A/B Bundle 硬件验证。
9. I1 已完成：新增 `firmware/common/iot_provisioning_worker`，用 RAM-only 状态机管理未来 provisioning-task 的 STOPPED/STARTING/RUNNING/STOPPING；当前不创建任务、不接入 `main.c`、不启动 SoftAP、不调用 Wi‑Fi/MQTT、不写 NVS。
10. I1 已通过主机 GCC analyzer、`-DNDEBUG`、cppcheck 和独立复审；复审提出的加锁契约已补入接口。默认 OFF/实验 ON 固件大小保持 `0x15f540`/`0x162150`。
11. I3 已完成：在普通 Wi‑Fi 路径中接入静态 mutex 和 `iot_wifi_owner_state_t`；owner 固定初始化为 NORMAL，当前没有路径 acquire PROVISION。
12. I3 已在 reconnect loop、RF health scan、boot watchdog、手动 disconnect/connect、Wi‑Fi stop/start、STA_START、STA_DISCONNECTED 和 STA_GOT_IP 前加入闸门；默认现网行为保持。
13. I3 默认 OFF 固件大小 `0x15fa80`，实验 ON `0x162660`，均剩余 4% 应用分区；独立代码复审 Approve。
14. 已完成 I4 前置的普通恢复 in-flight/quiesce 协调器：`firmware/common/iot_provisioning_recovery_quiesce`，可阻止配网 owner 获取期间普通长时扫描或破坏性恢复动作介入。
15. 协调器已覆盖操作登记、静默请求 BUSY/重试、破坏性尾部 fail-closed、取消、重置，以及 operation_id 全启动周期严格单调和不复用；独立复审 Approve。
16. 当前协调器尚未接入 `main.c`，默认 OFF/实验 ON 固件大小保持 `0x15fa80`/`0x162660`；下一步按小阶段将其接入 RF health scan，再考虑 owner acquire。

17. I4a 已完成：实验 ON 下 RF health scan 已接入恢复静默协调器；默认 OFF 现网行为不变。
18. 破坏性 deep-sleep 尾部已改为 coordinator 的显式 tail claim，在同一把配网锁内原子完成；只有 claim 成功才执行 recovery。
19. I4a 主机 analyzer/`NDEBUG`/cppcheck、完整默认 OFF/实验 ON 构建和独立复审均通过；实验 ON 固件为 `0x162a10`。下一步等待 14:06 长测结束后进入 CQ-4e 硬件短测。

### 阶段 B：验证 NVS A/B Bundle（需要硬件维护窗口）

1. 只烧录网关应用区，不刷 bootloader/分区表，不擦 Zigbee NVRAM；
2. 测试 save → load → restart → load；
3. 测试写入失败/取消/旧配置仍可用；
4. 验证离线接入的启动优先级：bundle > legacy keys > build default；
5. 验证异常 marker/CRC 回退和写入期间掉电恢复；
6. 验证通过后再考虑接入 SoftAP。

### 阶段 C：MQTT 安全部署（需要用户在场和维护窗口）

执行顺序：

1. 先部署“关闭匿名 + 新旧账号迁移共存 + ACL”；
2. 验证后端、网关、匿名拒绝和跨设备访问；
3. 保留短期回退能力；
4. 再移除旧账号；
5. TLS/WSS、证书和 VPN 后续由用户在场时教学式配置。

相关手册：

- `docs/ALIYUN_MQTT_ACL_DEPLOYMENT_RUNBOOK.md`
- `docs/ALIYUN_SERVER_MAINTENANCE_PLAN.md`

### 阶段 D：电池节点实测与硬件决策

1. 使用独立实验板烧录 `node_battery_sensor`，不得替换现网 COM5 组合节点功能；
2. 验证入网、PAN/Extended PAN ID、短地址、60 秒三项上报；
3. 用功率分析仪或低功耗电流表记录 sleep/poll/report 电流；
4. 区分 I2C 控制器、传感器 VCC、I2C 上拉、LED/USB 芯片等漏电；
5. 根据实测决定是否增加负载开关、调整 keep-alive/poll 或进入 deep sleep。

### 阶段 E：产品功能扩展

长稳定、凭据和电池实测之后再推进：

1. Web 控制界面的认证、TLS/VPN、CORS 和写操作权限；
2. “传感器 + 开关”组合设备能力模型；
3. 多节点短地址、能力、LQI、功率和命令状态；当前离线网关会拒绝非 selected RF node 的 RF 命令，防止全局 RF 状态误控；
4. 远距离/隔墙 ATPC 实测；
5. PCB、外壳和量产化设计。

## 4. 当前操作红线

- 未经用户明确同意，不切换网卡、不开关热点、不改安全组；
- 不 push、不部署或重启远程服务器，除非用户明确要求；
- 不修改阿里云 DNS、证书、MQTT Broker、TLS/WSS、VPN；
- 不整片擦 Flash、不擦 Zigbee NVRAM、不刷 bootloader/分区表；
- 不把电池实验固件烧入现网 COM5 组合节点，除非用户明确确认替换功能；
- 不读取、不打印、不落文档任何密码文件或真实凭据；
- 不使用 `git add -A`；
- 打开 COM5/COM6 前要意识到可能触发热复位。

## 5. 阿里云与后续运维备注

- 用户服务器为**阿里云 ECS**，公网 IP 在现有固件/工具配置中使用；
- MQTT 安全部署、TLS/WSS 和 VPN 暂不自动执行；
- 服务器资源需要保留余量，避免后端、MySQL、MQTT、Grafana 或日志占满磁盘/CPU；
- 所有部署必须准备回退步骤，并避开用户使用网络的时间。








