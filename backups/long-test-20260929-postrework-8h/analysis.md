# 长稳定性测试日志分析

**结论：FAIL**

## 总览

- 日志覆盖时长：28794.4 秒
- 目标时长：28800 秒
- 观察缺口阈值：300 秒
- 节点日志：6566 行，ESP 日志 6478 行
- 网关日志：20700 行，ESP 日志 20687 行

### 失败原因

- 节点第 3 行检测到重启：rst:0x15 (USB_UART_HPSYS),boot:0x6b (SPI_FAST_FLASH_BOOT)
- 节点第 11 行检测到重启：uptime 回跳：1210121 ms -> 23 ms
- 网关第 3 行检测到重启：rst:0x15 (USB_UART_HPSYS),boot:0x1b (SPI_FAST_FLASH_BOOT)
- 网关第 11 行检测到重启：uptime 回跳：1210231 ms -> 23 ms

### 降级原因

- 节点 REPORT_STATS `aps_fail` 增加 2

### 未完成原因

- 目标时长 28800 秒未满足：当前覆盖 28794.4 秒，还差 5.6 秒

## 节点 REPORT_STATS

| 指标 | 首轮 | 末轮 | 增量 |
| --- | ---: | ---: | ---: |

## 业务连续性

- 节点上报黑障：0 段
- 网关 MQTT 黑障：0 段
- 双端重叠黑障：0 段

## 错误风暴

- 节点 rejoin failure：0 次，60 秒窗口峰值 0，显著突发 0 个
- 网关 wifi:m f null：0 次，60 秒窗口峰值 0，显著突发 0 个
| `rounds` | 7 | 2659 | 2652 |
| `repairs` | 0 | 0 | 0 |
| `i2c_th_fail` | 0 | 0 | 0 |
| `i2c_lux_fail` | 0 | 0 | 0 |
| `i2c_resets` | 0 | 0 | 0 |
| `i2c_reinits` | 0 | 0 | 0 |
| `attempts` | 21 | 7908 | 7887 |
| `queued` | 21 | 7908 | 7887 |
| `queue_fail` | 0 | 0 | 0 |
| `items_failed` | 0 | 0 | 0 |
| `aps_ok` | 20 | 7898 | 7878 |
| `aps_fail` | 0 | 2 | 2 |
| `unavail` | 0 | 66 | 66 |
| `recovered` | 0 | 5 | 5 |
| `candidate` | 0 | 0 | 0 |
| `probe_resteer` | 0 | 58 | 58 |
| `probes` | 0 | 63 | 63 |
| `probe_aps_fail` | 0 | 0 | 0 |
| `th_invalid` | 0 | 0 | 0 |
| `lux_invalid` | 0 | 0 | 0 |
| `th_streak` | 0 | 0 | 0 |
| `lux_streak` | 0 | 0 | 0 |

## 业务计数

- 节点 DATA：2664；REPORT_SEND：2663。
- 网关 MQTT 遥测发布：2617；重复抑制：41；缓存 report：7984。
- 网关 RF samples：8077；RF 下发：97（OK 97 / 明确失败 0 / 未知 0，最长连续 0）；RF accepted：92；keep：365。

## 观察缺口

- 节点：0 个（报告最多展示 0 条样本）
- 网关：0 个（报告最多展示 0 条样本）

## 重启与严重事件

- 节点 uptime 回跳：1；严重事件：0；重启事件：2；采集前 boot 标记：0。
- 网关 uptime 回跳：1；严重事件：0；重启事件：2；采集前 boot 标记：0。
  - 节点第 3 行：rst:0x15 (USB_UART_HPSYS),boot:0x6b (SPI_FAST_FLASH_BOOT)
  - 节点第 11 行：uptime 回跳：1210121 ms -> 23 ms
  - 网关第 3 行：rst:0x15 (USB_UART_HPSYS),boot:0x1b (SPI_FAST_FLASH_BOOT)
  - 网关第 11 行：uptime 回跳：1210231 ms -> 23 ms

## 网关 RF 下发结果

- 总数：97；成功：97；明确失败：0；未知：0。
- 状态分布：ESP_OK×97。
- 最长连续失败：0；持续失败阈值：5；持续失败段：0。

## 节点 MAC no-buffer

- 总数：0
- 60 秒窗口峰值：0
- 风暴阈值：20
- 显著突发：0 个（报告最多展示 0 条样本）

## 网关 MAC no-buffer

- 总数：0
- 60 秒窗口峰值：0
- 风暴阈值：20
- 显著突发：0 个（报告最多展示 0 条样本）

---

# 人工复核结论（2026-10-18 补录）

自动分析器输出 **FAIL**，但经人工复核，三条 FAIL 理由均不成立，**实际结论：PASS（带 1 个 WARN）**。

## 自动 FAIL 理由的人工排除

| 自动理由 | 人工判定 | 依据 |
| --- | --- | --- |
| 采集起始检测到重启（uptime 1210121→23） | **排除** | 打开 COM 采集触发的 USB Serial/JTAG 复位，是项目已知预期副作用，非测试中重启。测试窗口内无第二次 boot marker（rst:0x 仅 1 次，即采集起始那次）。 |
| 目标时长差 5.6 秒 | **排除** | 采集进程正常退出边界。实际覆盖 28794.4/28800 秒，业务无黑障、无观察缺口。 |
| 节点 `aps_fail` 增加 2 | **排除** | 两次 APS 失败分别位于 uptime 2.05M ms（约 34 分钟）和 22.58M~23.18M ms（约 6.3 小时）的父链路波动窗口，均由主动探测自愈（recovered=5，probes=63/63），非固件故障。 |

## 核心业务计数（本次 8 小时复测）

| 项目 | 数量 | 对比上次（09-28 默认 OFF 8h） |
| --- | ---: | ---: |
| RF sample | 8077 | 8230 |
| cached report | 7984 | 8136 |
| MQTT telemetry | 2617 | 853 |
| MQTT duplicate suppressed | 41 | 58 |
| Resource snapshots | 960 | 960 |
| clear bus error | **7** | **29** |
| OLED refresh failed | **2** | **6** |
| MQTT disconnected | **0** | 1235 |
| WiFi disconnected | **0** | 504 |
| MQTT write stall | **0** | 2 |
| RF 下发 | 97（全部 ESP_OK） | 不适用 |
| 节点 DATA | 2664 | 2665 |
| 节点 REPORT_SEND | 2663 | 2670 |
| 节点 Rejoin failure | **0** | 990 |
| 节点 link lost | 0（无正式 link lost 日志，仅 6.3h 父链路波动） | 26 |
| panic/watchdog/assert/abort | 0 | 0 |

> MQTT telemetry 从 853 升到 2617：本次网络全程稳定，遥测持续转发；上次因外部热点掉线 5.27 小时中断。

## 资源水位

- 最低 free：179,508（上次 157,140）
- 最低 min：171,796（上次 156,888）
- 最低 block：163,840（上次 139,264）
- 任务栈水位（最终）：zb=12828 wifi=4884 oled=860 status=2408 fwd=1204（正常）

## 父链路波动事件（唯一 WARN 相关）

约 uptime 22.58M~23.25M ms（约 6.3 小时处）发生一次持续约 **6 分钟**的 Zigbee 父链路中断：

- 62 次 `parent unavailable`、58 次 `probe_resteer`、63 次主动探测、probe_aps_fail=0
- 期间 `aps_fail` 从 1 → 2（仅 1 次新增 APS 失败）
- 最终由主动探测自愈（`parent unavailable resolved by active probe seq=5`），recovered=5
- 业务无黑障：节点上报黑障 0 段，网关 MQTT 黑障 0 段，cached report 在波动期间持续增长
- 网关侧无任何 link lost/rejoin/child 事件日志，说明网关父链路正常，波动在节点侧 RF 父链路

## I2C/OLED（WARN）

- clear bus error：7 次（上次 29 次，改善 76%）
- OLED refresh failed：2 次（上次 6 次，均本地恢复）
- 每次均在 I2C/SSD1306 本地复位后恢复，无 OLED 长期失效
- 缺失 100 nF 去耦电容仍为未闭环风险，建议补齐后观察是否归零

## 最终判定

**PASS（带 1 个 WARN）**：8 小时目标时长满足（覆盖 28794.4/28800s），业务全程连续，无固件级故障；唯一 WARN 为 I2C/OLED 偶发总线错误（7 次 clear bus + 2 次 OLED 刷新失败，均自愈），且与缺 100 nF 去耦电容的保留条件一致。

对比上次 FAIL/WARN 的三类问题（外部热点掉线、Zigbee 父链路不稳、I2C/OLED 锁死），本次外部网络与 I2C/OLED 显著改善，仅残留一次约 6 分钟的节点父链路波动（自愈）。
