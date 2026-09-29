# 2026-09-28 晨间任务清单

## 2026-09-29 人工复核结论（最新，优先阅读）

长测已正常结束并完成人工复核：**FAIL/WARN（非单一固件故障）**。

1. 外部 PC 网卡/Windows 热点掉线：00:20 后 MQTT 异常，00:41 后 Wi-Fi AP 不可用，结束时仍未恢复；
2. Zigbee 父链路不稳定：22:43 已首次 Rejoin failure；长测内 990 行 Rejoin failure、26 次 link lost，最长中断 19 分 45 秒；
3. I2C/OLED：29 次 clear bus、6 次 OLED refresh failed，均本地恢复。

关键计数：RF sample `8230`、cached report `8136`、MQTT telemetry `853`、MQTT duplicate suppressed `58`、Resource snapshots `960`；最低 free heap `157140`。无 panic/watchdog/`ESP_ERR_NO_MEM`/brownout/意外重启。

下一步：保持当前固件；暂缓 Zigbee 事件队列拆分；先完成 OLED 接线/上拉/去耦，节点使用可靠数据线，再在稳定热点下做受控 8 小时复测。完整报告：`backups/long-test-20260928-default-8h/analysis.md`。


## 昨晚已经完成

1. 完成 P0-2：网关重启后先给 Zigbee 子节点 RF-quiet rejoin 窗口，短测通过。
2. 完成 P1-3：拆分 Zigbee 节点模型组件，短测通过。
3. 完成 P1-4：新增 Wi-Fi 恢复策略纯逻辑组件 `firmware/common/iot_wifi_recovery_policy/`。
4. 主机测试通过；默认 OFF、实验 ON 两种固件构建均通过。
5. 最终默认固件已只刷 COM6 应用区；未擦 Flash/NVRAM，未刷 bootloader/分区表。
6. 最终版 5 分钟硬件短测 PASS：Zigbee、Wi-Fi、MQTT、OLED/I2C、自动 RF 功率策略均正常。
7. 两轮代码复审 APPROVE。
8. 已建立本地 Git 检查点：
   - `e2e0d5f 拆分WiFi恢复策略组件`
   - `c8ebf3e 记录夜间长测启动状态`
9. 未 push、未部署服务器。

## 正在进行

- 01:39 的采集已确认没有保活、日志为空；已于 2026-09-28 21:38 重新启动 8 小时长测。
- 新窗口：COM5 节点 PID 2932，COM6 网关 PID 4644；守护进程 PID 31384。
- 预计 2026-09-29 05:38 结束，分析文件为 `backups/long-test-20260928-default-8h/analysis.md`。
- 启动约 917 秒：RF sample 272、cached report 269、MQTT telemetry 79、Resource snapshots 31；节点 DATA/REPORT_SEND 89。
- 当前警告：I2C clear bus error 5 次、OLED refresh failed 1 次且已恢复、NLME 4 次、parent unavailable 3 次且均快速恢复。
- 截至当前无 panic/watchdog、`ESP_ERR_NO_MEM`、MQTT 断连/写超时或 Rejoin failure；最终结论待完整日志。

## 事件拆分设计已提前完成

- 等待长测期间已完成只读分析与实施蓝图：`docs/ZIGBEE_EVENT_QUEUE_SPLIT_PLAN_2026-09-28.md`。
- 路线：P2-1 事件类型/构造/NLME 解析 → P2-2 队列投递封装 → P2-3 事件处理器（依赖注入）→ P2-4 forward task 收尾。
- 预计 main.c 减少约 600-750 行；长测 PASS 且确认后从 P2-1 开始，FAIL 则先排障。

## I2C 排障预分析已完成

- 长测期间已完成只读分析：`docs/I2C_CLEAR_BUS_ANALYSIS_2026-09-28.md`。
- clear bus error 为概率性从机/接线锁死，业务连续但按口径计 critical，长测预判 FAIL/WARN。
- 建议优先硬件整改（固定接线、4.7 kΩ 上拉、VCC 去耦），再决定是否固件调整。
## 明早建议处理顺序

1. 先确认两块板子仍供电、串口采集进程正常结束。
2. 生成长测分析报告；若守护未成功，再手动执行分析。
3. 若长测 PASS：更新交接文档并提交，然后继续下一项 Zigbee 应用事件拆分。
4. 若长测 FAIL：先定位异常发生时间和模块，不立即覆盖当前固件；优先分析周期性 I2C clear bus；必要时回退到上一个稳定提交。

## 可能需要你配合的事情

1. 醒来后先不要拔 COM5/COM6 USB 线，避免日志截断。
2. 看一眼 OLED 是否仍有正常显示；若出现周期性黑屏/刷新异常请告诉我。
3. 若长测期间设备掉线，请告诉我板子上的指示灯状态。
4. 如果今天要外出，优先让我先完成长测结论和回退点，再移动设备。
## 长测自动分析结果

自动分析已完成，结论：**FAIL/WARN**。

- 可观测跨度：约 `8.00` 小时
- RF sample：`8230`
- cached report：`8136`
- MQTT telemetry：`853`
- Resource snapshots：`960`
- 最低 free heap：`157140`
- OLED refresh failed：`6`
- 网关 critical：`{'MQTT disconnected': 1235, 'clear bus error': 29, 'OLED refresh failed': 6}`
- 节点 critical：`{'Rejoin failure': 990}`
- 完整分析：`backups/long-test-20260928-default-8h/analysis.md`
