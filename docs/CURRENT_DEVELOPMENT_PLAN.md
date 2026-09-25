# IoT-Home 当前开发计划

更新时间：2026-09-25 12:45（Asia/Shanghai；凭据校验阶段 1a 离线完成，等待硬件短测窗口）
事实来源：`docs/HANDOFF_2026-09-25.md`、`docs/HANDOFF/_2026-09-25.md`、`docs/HANDOFF_2026-09-24.md`

> 旧的 `docs/PROJECT_SUMMARY.md`、`docs/PROGRESS.md`、`docs/TODO_TOMORROW.md` 只作历史记录；新工作以本文件和最新交接为准。

## 1. 当前真实进度

系统是三层架构：Zigbee 节点 → ESP32-C6 网关 → Wi‑Fi/MQTT → FastAPI/MySQL/Grafana。

> 12:45 状态：凭据输入校验阶段 1a 已离线完成并取得复审 Approve；07:25 干净 4 小时长测仍为 DEGRADED，CQ-2a 默认 OFF，硬件短测前仍需重新复审。

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
   - 后端 RF policy REST API 和 Grafana 功率/模式面板已完成，但公网后端仍未部署这部分 API。

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
   - 当前尚未接入固件。

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
   - 当前阶段多次独立复审未返回有效结论，因此**未取得独立 Approve**。

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

默认 OFF 网关应用分区仅剩约 5%（约 `0x10ac0` 字节）；实验 ON 队列固件约剩 4%（`0xfb20` 字节）。SoftAP/HTTP、页面和校验逻辑继续叠加前必须先构建验收，不能直接加入完整 HTTP 配网功能。

### P1：NVS A/B 接口尚未硬件验证

A/B bundle 与启动路径已离线编译，但尚未验证真实硬件上的 save → restart → load、写入失败、异常 marker、掉电恢复和 legacy 回退。当前不得直接烧录或加入 SoftAP/HTTP。

### P1：RF 发送仍需完成 CQ-2b/CQ-3 集成

当前 generation guard 已在 COM6 运行，06:40 短序列未见重复帧或旧事务重放；正常物理拔节点也已通过。CQ-2a 已让实验 ON 路径中的用户命令通过队列消费者派发，但消费者暂时复用旧函数，周期/自动路径也尚未入队。后续仍需将远程、周期、自动任务接入同一消费者，由它负责预留序号、发送和安装等待事务。

CQ-2a 尚未取得有效独立 Approve；硬件烧录实验 ON 固件前必须重新复审。

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

### 阶段 A2：CQ-2a 已离线完成，下一步等待硬件短测

1. CQ-2a 已让实验 ON 路径中的用户 QUERY/SET/AUTO 先入队，再由 `rf-dispatch` 消费者派发；
2. 默认 OFF 现网路径不变；默认 bin `0x15f540`；
3. CQ-2b 再让消费者直接承担序号预留、Zigbee 发送和等待事务安装；
4. CQ-3 最后将周期/自动命令改为入队并停止旧直接发送路径；
5. 硬件短测按 `docs/RF_CQ_SHORT_TEST_RUNBOOK.md` 执行，判读使用 `tools/analyze_rf_cq_short_test.py`；
6. CQ-2a 未取得独立 Approve；硬件烧录或进入 CQ-2b 前必须重新复审；
7. 每一步单独构建、主机测试、提交；不得与 SoftAP/NVS A/B 运行时接入混做。

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
