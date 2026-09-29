# 2026-09-28 默认 OFF 8 小时夜间长测人工复核报告

- 自动分析生成：2026-09-29 05:38:10 +0800
- 人工复核时间：2026-09-29（当前分支 `codex/gateway-oled-wifi-checkpoint`）
- 长测目录：`C:\Users\HJB\Documents\iot-home\backups\long-test-20260928-default-8h`
- 结论：**FAIL/WARN（非单一固件故障；需分三类归因）**

## 1. 复核口径与外部条件

用户确认了两个长测外部因素：

1. **电脑网卡/Windows 热点中途掉线，之后才恢复**：该因素可解释网关侧 Wi-Fi/MQTT 可达性异常；
2. **节点中途或期间更换过另一条 USB 数据线**：可能影响供电、USB 转串口稳定或 RF 供电裕度。

日志证据显示：两块板子均只有打开采集时的一次 USB Serial/JTAG 复位；长测窗口内无第二次 boot marker。节点日志无 NUL/替换字符，且 commissioning/steering 日志在 20 分钟父链路异常期间持续输出，因此**没有证据表明更换数据线导致节点复位或采集进程断档**。但供电/RF 裕度影响无法仅由日志排除，后续应使用确认可靠的数据线做受控复测。

## 2. 运行窗口

- 计划起点：2026-09-28 21:38:02.757 +0800
- 计划时长：28800 秒 / 8 小时
- 网关最后日志 uptime：28793.681 秒（05:37:56）
- 节点最后日志 uptime：28794.820 秒（05:37:57）
- 网关文件采集时长：28795.7 秒；节点文件采集时长：28796.5 秒
- 可观测跨度：约 8.00 小时；两侧实际 boot marker 各 1 个
- 两个日志开头各有一小段打开端口前的残片，随后立即进入本次 USB Serial/JTAG 复位；正式计数均从本次复位后开始。

## 3. 核心业务计数（复位后）

| 项目 | 数量 |
| --- | ---: |
| RF sample | 8230 |
| cached report | 8136 |
| MQTT telemetry | 853 |
| MQTT duplicate suppressed | 58 |
| Resource snapshots | 960 |
| NLME status indication | 217 |
| clear bus error | 29 |
| OLED refresh failed | 6 |
| OLED refresh recovered | 6 |
| MQTT disconnected 原始日志行 | 1235 |
| WiFi disconnected 原始日志行 | 504 |
| Wi-Fi recovery stop/start | 58 |
| 网关 link/service unavailable | 8 |
| 网关 APS status=89 confirm | 8 |
| Device announced again | 8 |
| 节点 DATA | 2665 |
| 节点 REPORT_SEND | 2670 |
| 节点 parent unavailable 相关日志（201 次主事件 + 29 次 pending 中再触发） | 230 |
| 节点 parent resolved | 173 |

> `MQTT disconnected` 与 `WiFi disconnected` 是重复日志行，不等同于独立断连次数。MQTT 断连后客户端通常每 15 秒重试一次并重复记录。

## 4. 网络/MQTT 时间线（外部热点因素）

| 阶段 | uptime | 墙钟 | 持续时间 | 说明 |
| --- | ---: | --- | ---: | --- |
| 获取 IP | 9.241s | 21:38:12 | - | 网关获得 `192.168.137.196` |
| 初次 MQTT connected | 10.001s | 21:38:12 | - | 云端连接建立 |
| 第一次 MQTT 异常 | 9723.941s | 00:20:06 | - | socket write completion stall，随后记录 disconnected |
| 短暂恢复 | 9739.651s | 00:20:22 | 15.7秒 | 2 条 outbox 被 flush |
| 长故障开始 | 9829.251s | 00:21:52 | - | 再次 write stall/disconnected，之后无 connected |
| 最后一条成功 telemetry | 9809.861s | 00:21:32 | - | 之后云端遥测中断 |
| 首次 Wi-Fi 链路断开 | 11028.681s | 00:41:51 | - | reason=200（beacon timeout），热点/AP 已不可用 |
| 采集结束 | 28793.681s | 05:37:56 | 0.0秒 | 结束时仍未恢复 |

关键切分：

1. MQTT 首次异常到首次 Wi-Fi disconnect 相隔 **1304.7 秒**。该阶段未见应用层 Wi-Fi disconnect，说明更像热点仍有关联但上游网络/MQTT 可达性已经异常；
2. 从首次 Wi-Fi disconnect 到采集结束持续 **4.93小时 / 17765秒**，期间无重新获取 IP；期间 reason 分布：{'200': 1, '36': 289, '201': 214}；
3. MQTT 长故障从 00:21:52 到结束持续 **5.27小时 / 18964秒**；
4. 最后成功 telemetry 后缺口 **5.27小时 / 18984秒**。

网络重试现象：

- Wi-Fi recovery stop/start：58 次；
- `Host is unreachable`：1185 行；
- transport connect/open 失败：1233 行；
- TLS select timeout：47 行；
- socket write completion stall：2 行（00:20:06、00:21:52）；
- 未见专门的 `MQTT write timeout` 文本，但上述 2 行属于写入未在规定时间完成， refined 统计计入写超时/写停滞。

## 5. 节点 Zigbee 父链路时间线

### 5.1 `Rejoin failure` 聚类

ZBOSS 行自身不带 uptime，下表使用每行之前最近的应用 uptime 作为近似锚点。

| 聚类 | 近似 uptime 范围 | 墙钟范围 | 行数 |
| ---: | ---: | --- | ---: |
| 1 | 3929.950–3929.950s | 22:43:32–22:43:32 | 1 |
| 2 | 4002.220–4016.110s | 22:44:44–22:44:58 | 5 |
| 3 | 6248.860–6273.450s | 23:22:11–23:22:36 | 12 |
| 4 | 7002.030–7028.490s | 23:34:44–23:35:11 | 22 |
| 5 | 8156.940–8156.940s | 23:53:59–23:53:59 | 1 |
| 6 | 9401.440–9419.570s | 00:14:44–00:15:02 | 7 |
| 7 | 9888.270–11083.010s | 00:22:51–00:42:45 | 942 |

首次 Rejoin failure 发生在 22:43:32，比网关 MQTT 首次异常早约 5794 秒，因此不能归因于 PC 网卡掉线。

### 5.2 正式 link lost → connected episode

| # | link lost | connected | 持续 | 备注 |
| ---: | ---: | ---: | ---: | --- |
| 1 | 22:44:50 | 22:45:00 | 10.07秒 |  |
| 2 | 23:22:22 | 23:22:37 | 15.11秒 |  |
| 3 | 23:34:55 | 23:35:12 | 16.97秒 |  |
| 4 | 00:23:02 | 00:42:47 | 1185.33秒 | 最长父链路中断；传感器任务等待网络，故常规 DATA 暂停 |
| 5 | 00:47:51 | 00:47:52 | 1.01秒 |  |
| 6 | 00:51:01 | 00:51:02 | 1.02秒 |  |
| 7 | 00:52:44 | 00:52:45 | 1.02秒 |  |
| 8 | 00:54:55 | 00:55:01 | 6.04秒 |  |
| 9 | 01:01:12 | 01:01:13 | 1.01秒 |  |
| 10 | 01:03:08 | 01:03:09 | 1.01秒 |  |
| 11 | 01:45:42 | 01:45:48 | 6.04秒 |  |
| 12 | 02:00:10 | 02:00:16 | 6.05秒 |  |
| 13 | 02:13:31 | 02:13:32 | 1.02秒 |  |
| 14 | 02:20:09 | 02:20:10 | 1.01秒 |  |
| 15 | 02:24:08 | 02:24:09 | 1.01秒 |  |
| 16 | 02:28:17 | 02:28:18 | 1.02秒 |  |
| 17 | 02:29:37 | 02:29:38 | 1.01秒 |  |
| 18 | 02:52:44 | 02:52:45 | 1.02秒 |  |
| 19 | 02:54:24 | 02:54:25 | 1.01秒 |  |
| 20 | 02:57:07 | 02:57:08 | 1.01秒 |  |
| 21 | 03:04:44 | 03:04:45 | 1.02秒 |  |
| 22 | 03:31:12 | 03:31:13 | 1.01秒 |  |
| 23 | 04:18:01 | 04:18:02 | 1.02秒 |  |
| 24 | 04:21:50 | 04:21:51 | 1.01秒 |  |
| 25 | 05:15:11 | 05:15:12 | 1.02秒 |  |
| 26 | 05:23:19 | 05:23:20 | 1.02秒 |  |

正式 link lost 共 26 次；中断总时长约 1264.9 秒，其中最长一次 1185.3 秒。后续多数 episode 可在 1–6 秒恢复，但频繁发生说明父链路裕度/协议状态仍不稳定。

### 5.3 节点 REPORT_STATS 增量

```text
rounds: +2657
repairs: +5
i2c_th_fail: +0
i2c_lux_fail: +0
i2c_resets: +0
i2c_reinits: +0
attempts: +7969
queued: +7955
queue_fail: +14
items_failed: +4
aps_ok: +7685
aps_fail: +80
unavail: +237
recovered: +173
probe_resteer: +26
probe_aps_fail: +30
th_invalid: +0
lux_invalid: +0
th_streak: +0
lux_streak: +0
```

补充：显式 `parent unavailable event` 日志为 201 次，另有 29 次“probe verification pending 期间再次 parent unavailable”；原始 parent unavailable 文本合计 230。最终 REPORT_STATS 中 `unavail=237`，以节点累计统计为准；`recovered=173` 与显式恢复日志一致。
节点本地传感器 I2C：`i2c_th_fail=0`、`i2c_lux_fail=0`、无总线 reset/reinit。

## 6. I2C/OLED 独立问题

- clear bus error：29 次；
- OLED refresh failed：6 次；
- 每次失败均通过本地 I2C/SSD1306 reset 恢复，恢复日志 6 次；
- 事件主要集中在 21:38–00:20，另有一次 03:06 的 clear bus；
- 该问题与 PC 网卡掉线无直接时间对应，也未造成 Zigbee/MQTT 业务长期中断。

优先硬件整改：固定并缩短 SDA/SCL、增加 4.7 kΩ 上拉、OLED VCC 就近增加 100 nF 去耦；整改前不建议在 Flash 仅剩约 54 KB 的固件中盲目叠加处理逻辑。

## 7. 资源水位与任务栈高水位

- Resource snapshots：960，约每 30 秒一条；
- 最终 free heap：184888；最低 free heap：157140（00:27:36）；
- reported minimum heap：最终 156888，历史最低 156888；
- largest free block：最终 163840，最低 139264（00:26:06）。

| 任务 | 最终剩余栈 | 启动后最低剩余栈 | 最低出现时间 |
| --- | ---: | ---: | ---: |
| zb | 12828 | 12828 | 22:59:03 |
| wifi | 1912 | 1912 | 00:42:06 |
| oled | 860 | 860 | 21:38:33 |
| status | 2312 | 2312 | 23:11:33 |
| fwd | 856 | 856 | 00:20:06 |

## 8. 异常扫描

| 异常 | 网关 | 节点 |
| --- | ---: | ---: |
| panic | 0 | 0 |
| Guru Meditation | 0 | 0 |
| watchdog | 0 | 0 |
| ESP_ERR_NO_MEM | 0 | 0 |
| brownout | 0 | 0 |
| abort() | 0 | 0 |
| assert failed | 0 | 0 |
| Rejoin failure | 0 | 990 |
| MQTT write completion stall | 2 | 0 |

未发现 panic、watchdog、`ESP_ERR_NO_MEM`、brownout、assert failed；未发生意外重启。

## 9. 每小时分布

### 网关

| 指标 | 0h | 1h | 2h | 3h | 4h | 5h | 6h | 7h |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| RF sample | 1042 | 1012 | 762 | 1049 | 1088 | 1125 | 1078 | 1074 |
| cached report | 1030 | 998 | 753 | 1037 | 1076 | 1113 | 1067 | 1062 |
| MQTT telemetry | 312 | 312 | 229 | 0 | 0 | 0 | 0 | 0 |
| MQTT duplicate | 28 | 18 | 12 | 0 | 0 | 0 | 0 | 0 |
| NLME | 7 | 9 | 5 | 41 | 41 | 41 | 34 | 39 |
| clear bus | 10 | 13 | 5 | 0 | 0 | 1 | 0 | 0 |
| OLED failed | 2 | 2 | 2 | 0 | 0 | 0 | 0 | 0 |
| MQTT断连行 | 0 | 0 | 40 | 234 | 240 | 241 | 240 | 240 |
| WiFi断连行 | 0 | 0 | 0 | 99 | 102 | 101 | 101 | 101 |
| WiFi重启 | 0 | 0 | 0 | 11 | 12 | 11 | 12 | 12 |

### 节点

| 指标 | 0h | 1h | 2h | 3h | 4h | 5h | 6h | 7h |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| DATA | 341 | 335 | 255 | 330 | 349 | 359 | 349 | 347 |
| REPORT_SEND | 341 | 338 | 257 | 330 | 349 | 359 | 349 | 347 |
| parent unavailable | 5 | 21 | 25 | 49 | 37 | 47 | 24 | 22 |
| parent resolved | 5 | 5 | 7 | 43 | 30 | 42 | 21 | 20 |
| link lost | 0 | 3 | 1 | 6 | 7 | 5 | 2 | 2 |
| connected | 1 | 4 | 3 | 7 | 7 | 5 | 2 | 2 |
| Rejoin failure | 0 | 40 | 733 | 217 | 0 | 0 | 0 | 0 |

## 10. 最终判定与下一步

本次长测不判为纯固件 PASS，原因包括：

1. **外部网络/热点故障**：造成 5 小时以上 MQTT/Wi-Fi 不可达，需从固件故障中单独切分；
2. **Zigbee 父链路稳定性不足**：首次异常早于外网故障，出现 990 行 Rejoin failure、26 次正式 link lost 和一次 19 分 45 秒中断；
3. **网关 I2C/OLED 概率性总线锁死**：29 次 clear bus、6 次 OLED refresh failed，虽均恢复但按口径计 critical。

下一步不覆盖当前固件、不实施 P2 Zigbee 事件队列拆分：

1. 保持当前检查点 `1eb4afe` 和当前默认 OFF 固件；
2. 用户确认网络已稳定后，如需观察当前实时状态，再单独决定是否打开串口（打开可能复位）；
3. 完成 OLED 接线/上拉/去耦硬件整改，节点使用确认可靠的数据线；
4. 在网络热点持续稳定、节点数据线确认可靠的条件下，安排一次受控 8 小时复测；
5. 若受控复测仍复现父链路异常，再按时间线深入排查 ZBOSS polling/rejoin、child aging、coexistence 和供电，不先叠加新功能。
