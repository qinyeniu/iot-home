# 2026-09-28 默认 OFF 8 小时夜间长测分析

生成时间：2026-09-29 05:38:10 +0800

结论：**FAIL/WARN**

## 运行窗口

- 网关 uptime：23 → 28793681 ms
- 节点 uptime：23 → 28794820 ms
- 网关实际文件采集时长：28795.7 秒
- 节点实际文件采集时长：28796.5 秒
- 计划时长：28800 秒 / 8 小时
- 可观测跨度：约 28794.8 秒 / 8.00 小时

## 网关计数

- RF sample: 8230
- cached report: 8136
- MQTT telemetry: 853
- MQTT duplicate suppressed: 58
- Resource snapshots: 960
- RF power query accepted: 93
- RF power keep: 362
- APS confirm OK: 86
- NLME status: 217
- boot markers: 1
- OLED refresh failed: 6
- OLED recovered by reset: 6
- OLED refresh recovered: 6
- clear bus error: 29
- MQTT disconnected events: 1235
- MQTT write timeout events: 0
- Rejoin failure events: 0

## 节点计数

- DATA: 2665
- REPORT_SEND: 2670
- REPORT_STATS: 426
- parent unavailable: 230
- parent recovered: 173
- boot markers: 1

## 节点 REPORT_STATS 增量

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
candidate: +0
probe_resteer: +26
probes: +225
probe_aps_fail: +30
th_invalid: +0
lux_invalid: +0
th_streak: +0
lux_streak: +0
```

## 最终资源快照

```text
free=184888
min=156888
largest_block=163840
stack_free[zb=12828 wifi=1912 oled=860 status=2312 fwd=856]
历史最低堆（资源日志）=156888
```

## 全部资源快照统计

```text
snapshots=960
free heap: final=184888 min=157140
reported min heap: 156888
largest block: final=163840 min=139264
reset_reasons={'usb': 960}
min_stack_free[zb=12828 wifi=1912 oled=860 status=2312 fwd=856]
final_stack_free[zb=12828 wifi=1912 oled=860 status=2312 fwd=856]
```

## 异常/警告

- 网关 critical: {'MQTT disconnected': 1235, 'clear bus error': 29, 'OLED refresh failed': 6}
- 节点 critical: {'Rejoin failure': 990}
- 启动标记/意外重启: 无
- 网关 warning: {'NLME status indication': 217}
- 节点 warning: {'parent unavailable event': 230}
- 计数不足: 无

## 判定依据

- 无 critical、计数达到最低预期时为 PASS；
- 仅有父链路/NLME 瞬时提示但业务连续时为 PASS_WITH_WARNINGS；
- 出现 panic/watchdog/内存/MQTT/I2C/OLED 异常或样本显著不足时为 FAIL/WARN。
