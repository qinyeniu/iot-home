# 2026-09-29 OLED 硬件整改后 20 分钟受控短测人工复核报告

- 短测时间：2026-09-29 22:12–22:32（约 20 分钟）
- 当前分支：`codex/gateway-oled-wifi-checkpoint`
- 短测目录：`backups/hw-post-rework-short-20260929/`
- 采集方式：复用 `tools/capture_serial_no_reset.py`，不主动脉冲 RTS/DTR；打开端口触发各一次 USB Serial/JTAG 复位
- 结论：**基本通过（PASS with 1 WARN）**——业务全程连续；仅启动早期 1 次 `clear bus error`，无 OLED 整帧失败

## 1. 整改前提

用户已确认：

1. OLED SDA/SCL 缩短并固定；
2. SDA/SCL 各增加 4.7 kΩ 外部上拉到 3.3V；
3. 节点更换确认可靠的 USB 数据线；
4. Windows 热点和外网稳定。

保留项：100 nF 去耦电容此前确认暂缺，本次测试在“无就近去耦”条件下进行。

## 2. 业务计数

| 项目 | 数量 |
| --- | ---: |
| RF sample | 343 |
| cached report | 339 |
| MQTT telemetry | 112 |
| MQTT duplicate suppressed | 1 |
| Resource snapshots | 41 |
| 节点 DATA | 113 |
| 节点 REPORT_SEND | 113 |

交叉验证：节点约每 10.6 秒上报一次，1200 秒 ≈ 113 次；每次 3 个簇，RF sample ≈ 113×3；MQTT 侧 112 次新发 + 1 次重复抑制 = 113；Resource 约每 30 秒一条，首尾共 41 条。计数自洽。

## 3. 启动/连接时间线

| uptime | 事件 |
| ---: | --- |
| 331 ms | 网关 I2C init OK（SDA=2 SCL=3） |
| 431 ms | OLED init OK（SSD1306 文本驱动） |
| 3591 ms | 节点入网（addr=0x82cb） |
| 5991 ms | 完整温/湿/光样本就绪，启动 Wi‑Fi |
| 12701 ms | 获取 IP `192.168.137.91` |
| 13361 ms | MQTT connected（broker `8.163.110.27:1883`） |

说明：RF-quiet rejoin 机制按设计工作——先 Zigbee 入网并拿到完整样本，再启动 Wi‑Fi/MQTT。网关本次地址为 `192.168.137.91`（长测期间为 `.196`，热点恢复后重新分配）。

## 4. 唯一 WARN：启动早期 1 次 clear bus error

```text
E (11591) i2c: clear bus error
```

关键事实：

1. 仅 **1 次**，发生在启动后约 11.6 秒；
2. 紧邻 Wi‑Fi 关联/ADDBA 握手（11371–11401 ms）与获取 IP（12701 ms）之间，处于启动联网高峰；
3. **没有**记录 `OLED refresh failed`，说明在帧内重试阶段即恢复，未走到完整本地恢复；
4. 此后约 1188 秒（至测试结束）再无任何 I2C 文本，OLED 每秒刷新持续成功；
5. 网关、节点全程各只有 1 个 boot marker，无第二次复位。

判断：这是启动瞬态事件，不排除与 Wi‑Fi 初始关联时的供电/总线状态有关；在缺少 100 nF 就近去耦的条件下，该因素无法完全排除。它未影响显示和业务连续性，但按严格口径仍记为 1 条 WARN。

## 5. Zigbee 父链路：零异常

节点全部 REPORT_STATS（每约 64 秒一条）持续保持：

```text
unavail=0  recovered=0  aps_fail=0  queue_fail=0
probe_resteer=0  probes=0  i2c_th_fail=0  i2c_lux_fail=0
```

全程无 `Rejoin failure`、`link lost`、`parent unavailable`。对比 8 小时长测（990 行 Rejoin failure、26 次 link lost、最长中断 19 分 45 秒），本次父链路稳定。

## 6. Wi‑Fi/MQTT：零断连

- 无 `MQTT disconnected`、`WiFi disconnected`、beacon timeout；
- 无 write completion stall；
- MQTT 自 13361 ms 连接后持续在线，telemetry 连续到结束。

## 7. 资源水位

```text
Resource 行数: 41
free heap  最低 179776 / 最终 181004
min heap   最低 173892 / 最终 173892
largest block 最低 159744 / 最终 163840
```

最终栈水位：`zb=12908 wifi=4880 oled=860 status=2408 fwd=1224`。

free heap 在约 181000 附近稳定，无持续下降趋势；无 panic、watchdog、brownout、`ESP_ERR_NO_MEM`、assert failed 或意外重启。

## 8. 与上一轮 8 小时长测对比

| 指标 | 8h 长测 | 本次 20 分钟短测（换算口径） |
| --- | ---: | ---: |
| clear bus error | 29 | 1（启动瞬态） |
| OLED refresh failed | 6 | 0 |
| Rejoin failure | 990 | 0 |
| link lost | 26 | 0 |
| MQTT/Wi‑Fi 断连 | 5h+ | 0 |

整改效果显著：外部上拉与短接线后，I2C clear bus 从长测的高频出现降为启动期 1 次且帧内恢复；父链路与网络在稳定热点下零异常。

## 9. 判定与下一步

判定：**PASS with 1 WARN**，业务连续性、父链路、网络和资源水位全部满足短测要求。

建议（需用户决定）：

1. 更稳妥：先补齐 OLED VCC 就近 100 nF 去耦电容，再安排受控 8 小时复测，以排除启动瞬态 clear bus 的供电因素；
2. 若用户接受“缺电容 + 启动期 1 条 WARN”的保留条件，也可以直接安排受控 8 小时复测；
3. 若 8 小时复测中 clear bus 再次在非启动时段出现或频率升高，则降低 I2C 速率 / 调整刷新周期，并继续排查 OLED 模块与供电；
4. 本次短测不改变固件：未编译、未刷写、未改配置、未擦 Flash/NVRAM。