# Zigbee 自动发射功率控制（ATPC）设计

**更新时间**：2026-09-23
**状态**：代码已实现并烧录；自动功率上下限、运行时免烧录查询/手动固定/恢复自动、严格事务边界均已通过硬件实测
**适用设备**：ESP32-C6 传感器+继电器二合一节点、ESP32-C6 网关

## 1. 最终设计结论

节点功率不再按摆放位置手工硬编码。系统采用“**安全功率启动 + 真实失败自动提升 + 长期富余自动降低 + 确认后更新状态**”策略：

1. 节点每次上电先使用已验证的近距离安全功率 **-10 dBm**；
2. 远距离导致入网/secure rejoin 失败时，节点经过 45 秒和 4 次失败观察后提升一档；
3. 入网后只有真实 APS 发送失败达到阈值，节点才提升一档；
4. LQI 和延迟读取的 RSSI **不单独作为升功率依据**；
5. 网关在加入/重加入后主动 QUERY，只有收到节点加密 REPORT 才更新当前功率；
6. 只有长期高 LQI 且当前功率高于 -10 dBm，网关才降低一档，避免学习到不必要的高功率。

因此移动节点后不需要每次改代码。远位置通常会在数十秒到数分钟内根据实际失败自动爬升；回到近距离后重新上电会恢复 -10 dBm，安全启动优先。

默认安装策略给自动控制留出的范围是 **-10 dBm 到 +20 dBm**。这个范围是“允许自动调节的边界”，不是让节点一直以 +20 dBm 发射：链路稳定时仍保持 -10 dBm，只有真实失败累积才向上升。若现场确认不需要这么高，也可以一次性把上限降到 +8 或 +14 dBm。

## 2. 为什么不能只按“距离”设置固定功率

Zigbee 实际链路质量不仅受直线距离影响，还受以下因素影响：

- 墙体材质、金属物体、家具遮挡；
- 节点和网关天线方向；
- 周边 2.4 GHz Wi‑Fi/蓝牙/Zigbee 干扰；
- 板子摆放高度、朝向和反射路径；
- 近距离高功率是否造成接收端饱和。

距离只能用于粗略估算，工程控制以实际入网结果和 APS 成功/失败为准。

## 3. 功率档位与经验范围

ESP32-C6 芯片可支持约 **-15 dBm 到 +20 dBm**。当前默认自动策略使用 6 档，默认允许范围为 **-10 dBm 到 +20 dBm**；-15 dBm 只保留给后续低功耗实验。

| 档位 | 功率 | 典型场景 | 自动控制含义 |
|---|---:|---|---|
| 0 | **-10 dBm** | 同桌面、0–2 m 近距离联调 | 固定安全启动值，防止网关接收端饱和 |
| 1 | **0 dBm** | 同房间数米 | 近距离失败后的第一档提升 |
| 2 | **+8 dBm** | 同房间较远或轻度遮挡 | 常见室内可靠性平衡值 |
| 3 | **+14 dBm** | 隔一堵轻墙/较远位置 | 链路质量不足时提升 |
| 4 | **+18 dBm** | 多墙、遮挡明显 | 高可靠性优先 |
| 5 | **+20 dBm** | 最远或遮挡严重位置 | 当前硬件最大功率 |

人工估算时可参考：

| 安装情况 | 建议起始范围 |
|---|---:|
| 同桌面/0–2 m | -10 dBm |
| 同房间 2–8 m | 0 到 +8 dBm |
| 隔一堵墙/8–15 m | +8 到 +14 dBm |
| 多堵墙或更远距离 | +18 到 +20 dBm |

**-15 dBm 暂不纳入自动表**：当前已验证 -10 dBm 可避免近距离异常；-15 dBm 留给后续低功耗实验，不在未验证情况下自动使用。

### 3.1 一次性配置自动功率范围

当前不需要为每个摆放位置改代码，只在确定安装策略后按需修改一次边界：

- 节点文件：`firmware/node_zigbee/main/main.c`
  - `ZIGBEE_NODE_TX_POWER_START_DBM`：每次上电的安全启动功率，默认 `-10`；
  - `ZIGBEE_NODE_TX_POWER_AUTO_MIN_DBM`：自动控制允许的最低功率，默认 `-10`；
  - `ZIGBEE_NODE_TX_POWER_AUTO_MAX_DBM`：自动控制允许的最高功率，默认 `20`。
- 网关文件：`firmware/gateway/main/main.c`
  - `RF_POWER_AUTO_MIN_DBM`：网关允许/下发的最低功率；
  - `RF_POWER_AUTO_MAX_DBM`：网关允许/下发的最高功率。

这些数值必须**精确等于**功率表中已有的档位；节点和网关的配置要保持一致。固件已加入编译期检查，如果填入 `-5`、`+4` 这类表外数值，不会再静默就近映射，而是直接编译失败。修改边界后需要重新编译并分别烧录节点、网关。

推荐预设：

| 使用方式 | 启动功率 | 自动下限 | 自动上限 | 说明 |
|---|---:|---:|---:|---|
| 开发调试/节点经常移动 | -10 dBm | -10 dBm | +20 dBm | 当前默认，优先保证不同位置都能自动适应 |
| 固定在同房间 | -10 dBm | -10 dBm | +8 或 +14 dBm | 减少不必要的高功率和耗电 |
| 固定隔墙/较远位置 | -10 dBm | -10 dBm | +20 dBm | 仍建议安全启动，让失败驱动自动爬升 |
| 未来电池低功耗实验 | -15 dBm | -15 dBm | +8 dBm | 需先完成专门实测，当前不作为默认 |

如果节点只是临时换位置，不建议改这些配置；默认 `-10..+20` 已能覆盖。只有节点长期固定在某类环境，才需要调整上限。

## 4. 节点侧启动与自动提升

### 4.1 启动策略

节点启动时不直接复用 NVS 中上次学习到的高功率。即使上一次在远处学习到 +20 dBm，移动到网关旁边后也不会以高功率启动。

```text
固定启动：-10 dBm
```

学习到的功率仍写入 NVS，仅用于现场记录和后续分析；启动时不复用。

### 4.2 入网失败提升

节点无法入网或 secure rejoin 失败时：

- 至少累计 4 次失败；
- 且距离第一次失败已满 45 秒；
- 才提升一档；
- 每次提升后重新计数，仍失败则再等 45 秒继续提升。

完整爬升时间示例：

```text
-10 dBm 启动
约 45 秒后 -> 0 dBm
再约 45 秒 -> +8 dBm
再约 45 秒 -> +14 dBm
再约 45 秒 -> +18 dBm
再约 45 秒 -> +20 dBm
```

这可以避免刚启动时的短暂失败导致功率爬升过快。

### 4.3 运行中 APS 失败提升

节点正常入网后，普通传感器 APS 帧失败不会立即提高功率。提升条件为：

- 60 秒窗口内至少 4 次 APS 失败；
- 已成功连接并稳定至少 30 秒；
- 距上一次本地自动提升至少 90 秒；
- 当前不是最高档。

每轮传感器有 3 个属性帧，4 次失败意味着异常通常跨越多个上报批次，而不是单个启动瞬断。成功帧不会擦除窗口内已经发生的失败；窗口超时后计数自然重启。

## 5. 网关侧 QUERY / REPORT

网关使用私有制造商范围簇 `0xFC10` 与节点通信。

### 5.1 加入或重加入后必须重新确认

同一个短地址 `0x82cb` 可能对应：

- 全新加入；
- 同一节点复位；
- 上一次高功率进程结束、新进程以 -10 dBm 启动。

因此网关在 join/announce 事件后必须丢弃旧功率、旧 LQI 和等待状态，并立即进入 QUERY 流程。不能把上一次运行的 +18 dBm 错误套到新启动节点上。

流程：

```text
网关 QUERY
  -> 节点用 REPORT 返回实际功率
  -> 网关校验通过后更新 s_rf_node_power_dbm
```

### 5.2 SET/QUERY 超时

队列接受 Zigbee 请求不代表节点已经处理。网关发送 QUERY/SET 后等待加密 REPORT：

- 90 秒未确认则判定本次等待超时；
- deadline 不只在周期任务中检查：REPORT 实际到达时会按当前时间再次判断；
- 超时后，即使迟到 REPORT 的序号、功率、模式都正确，也不能再确认原用户事务；
- 超时后重新 QUERY 当前实际功率；
- 不盲目重发 SET，防止节点其实已经应用但 REPORT 丢失；
- 链路陈旧只会终止本次用户事务，持久保存的期望模式/手动功率不会丢失，链路恢复后自动对账。

### 5.3 LQI 的使用边界

本硬件实测发现：

- 节点 -10 dBm、近距离 APS 全部成功时，APSDE indication 中 LQI 只有约 **20–30**；
- 功率 +8 dBm 时 LQI 约 **120**；
- 功率 +14 dBm 时 LQI 约 **147–153**；
- 在回调稍后读取 `esp_ieee802154_get_recent_rssi()`，可能得到 **-90 到 -101 dBm** 的陈旧/共存值，但当时帧实际成功。

因此：

- LQI 只作为诊断和“长期富余时降低功率”的依据；
- 弱 LQI 本身不触发提升；
- 延迟读取 RSSI 不参与控制。

### 5.4 降低条件

- LQI EMA 高于 **240**；
- 连续 **3 个** 60 秒窗口满足；
- 当前功率高于最低启动档；
- 网关发送 SET 将节点降低一档。

近距离当前 -10 dBm 时 LQI 约 20–30，不会误降；由失败提升到高功率且长期链路确有明显富余时才会降档。

## 6. 安全与边界

- 私有帧使用 Zigbee 安全链路；
- 节点只接受短地址 `0x0000`、endpoint/profile 匹配且安全状态有效的网关命令；
- 网关校验 profile、endpoint、帧魔数、版本、长度和 REPORT 序号；
- 节点请求值会映射到内置功率表，不接受表外任意功率；
- 私有帧被回调消费，不影响标准传感器和 OnOff cluster；
- 不刷 bootloader/分区表，不擦 Zigbee NVRAM。

## 7. 2026-09-23 硬件验证

### 已完成

- 节点固件编译通过；
- 网关固件编译通过；
- 网关 QUERY 能收到节点 REPORT；
- 近距离启动时 REPORT 正确返回 -10 dBm；
- 约 5 分钟内网关在 121/183/245/307 秒评估点均保持 -10 dBm。

### 5 分钟节点统计

- rounds=25；
- attempts=75、queued=75；
- queue_fail=0、items_failed=0；
- aps_fail=0；
- unavail=0、recovered=0、probe_resteer=0；
- I2C failures/resets/reinits=0。

### 实测日志（01:12 基线）

- 节点：`firmware/node_zigbee/log.atpc-failure-driven-node-20260923011224.txt`
- 网关：`firmware/gateway/log.atpc-failure-driven-gw-20260923011144.txt`
- 固件备份：`backups/firmware/2026-09-23-failure-driven-atpc/`

### 烧录后复验（02:06，约 3 分钟，网关保持运行、仅复位节点）

- 节点烧录 COM5、网关烧录 COM6 后均正常启动；
- 节点最终统计：`aps_fail=0`、`queue_fail=0`、`items_failed=0`、I2C failures=0；
- 期间出现 1 次 `parent unavailable`，独立探针立即成功，`recovered=1`，没有触发误升功率；
- 网关在多个评估点记录 `RF power: keep -10 dBm`，LQI EMA 约 24–25；
- MQTT 遥测持续到达。

日志：

- 节点：`firmware/node_zigbee/log.atpc-bounds-nodeonly-202609230206.txt`
- 网关：`firmware/gateway/log.atpc-bounds-gw-nodeonly-202609230206.txt`
- 固件备份：`backups/firmware/2026-09-23-configurable-atpc-bounds/`

### 已知边界：节点/网关同时冷启动

烧录后的同启观察中，节点前约 106 秒出现过一次父链路验证不一致，随后自动 resteer 并恢复，恢复后持续保持 -10 dBm。因此当前策略可以自恢复，但同启时的完全收敛可能需要约 2 分钟。后续可优化为网关先启动、节点延迟加入，或进一步缩短验证/重导时间。

## 8. 运行时免烧录修改

除编译期边界外，当前已经支持在设备运行过程中通过 MQTT 修改策略，不需要为“换一个功率”重新编译或烧录固件。网关订阅节点命令主题：

```text
iot-home/gw-001/nodes/zb-82cb/cmd
```

支持命令：

```json
{"command":"rf_query_power"}
{"command":"rf_set_power","payload":{"power_dbm":0}}
{"command":"rf_manual_power","payload":{"power_dbm":8}}
{"command":"rf_set_auto"}
```

功率值仍只能选择内置档位：`-10、0、8、14、18、20 dBm`。网关发起 QUERY 时先发布 retained `status:"querying"`；发送加密 Zigbee SET/QUERY 后，不会把“协议栈接受发送请求”当作成功；只有收到节点加密 REPORT，且命令序号、功率和模式均匹配，才更新 retained 状态：

```text
iot-home/gw-001/nodes/zb-82cb/rf_policy
```

本地工具：

```powershell
server/backend/.venv/Scripts/python.exe tools/rf_power_config.py --host 8.163.110.27 query
server/backend/.venv/Scripts/python.exe tools/rf_power_config.py --host 8.163.110.27 set --power 0
server/backend/.venv/Scripts/python.exe tools/rf_power_config.py --host 8.163.110.27 auto
```

工具会生成一次性 `request_id`，网关在 querying/confirmed 状态中原样带回；工具只接受相同 `request_id`、`status:"confirmed"` 且模式/功率匹配的状态，避免订阅时先读到旧 retained 值而误判成功。

### 8.1 手动模式的控制含义

- `rf_set_power` 会让节点进入 `manual` 模式；
- 节点在手动模式下不会因为真实 APS 失败或入网失败自动升高功率；
- `rf_set_auto` 会保持当前功率并切换回自动模式，后续失败才允许升功率；
- 想立即回到近距离安全值，可先设置 `-10 dBm`，再恢复自动。

### 8.2 v1/v2 兼容边界

当前私有协议 v2 为 8 字节，在 v1 6 字节基础上增加 `mode` 和 `flags`：

```text
magic0 magic1 version command sequence power mode flags
```

新网关/新节点可以接收 v1 输入，但 v1 REPORT 没有模式字段，不能证明手动模式已锁定。因此手动命令必须收到 v2 的 `mode=manual` REPORT 才算成功；本项目不保证 v1 网关与 v2 节点、v2 网关与 v1 节点的完整混合运行。

### 8.3 事务关联

MQTT 命令可携带：

```json
{"command":"rf_set_power","request_id":"a1b2c3d4e5f60718","payload":{"power_dbm":0}}
```

网关将当前事务 ID 保存在 `s_rf_expected_request_id` 中，QUERY/SET 的 confirmed policy 必须带回同一 ID。网络 reset/leave 会清空该 ID。这样即使旧 retained 报文刚好具有同样功率，也不会被当作本次命令的确认。

严格规则：

- 网关命令响应使用低序号 `1..127`；
- 节点主动上报使用高序号 `0x80..0xFF`；
- 主动上报永远不能携带本次命令的 `request_id` 充当确认；
- 只有 v2 REPORT 的序号、功率、模式均匹配当前等待事务，且 `expected_user=true` 时才确认；
- 幂等场景下若节点状态本来就已满足请求，最终发布 confirmed 前还会在锁内二次校验全局事务，避免抢占并发来的新命令被旧 `request_id` 误确认。

## 9. 2026-09-23 运行时策略硬件复验

- 冷启动时网关先连 Wi‑Fi/MQTT，约 15.2 秒启动 Zigbee，节点约 16.2 秒首次 announce；
- 由于串口打开导致近乎同冷启动，节点前 10 个父链路探针 APS 失败，随后自动 resteer；约 47.5 秒重新稳定；
- QUERY 返回 `auto/-10 dBm`；
- 带 request_id 设置固定 `0 dBm` 返回 `manual/0 dBm`，节点 LQI 从约 25–30 变为约 86–91；
- 恢复自动后保持当前 `0 dBm`；再设置 `-10 dBm` 并恢复自动，最终为 `auto/-10 dBm`；
- 后续至 230 秒持续保持 -10 dBm，普通遥测持续，节点最终统计中正常业务 `aps_fail=0、queue_fail=0、items_failed=0、I2C failures=0`。

日志：

- 节点：`firmware/node_zigbee/log.runtime-rf-policy-node-024423.txt`
- 网关：`firmware/gateway/log.runtime-rf-policy-gw-024423.txt`
- 加入 request_id 后重新烧录网关，最终 QUERY、SET 0、AUTO、SET -10、AUTO 均只确认本次事务，最终为 `auto/-10 dBm`；
- 固件备份：`backups/firmware/2026-09-23-runtime-rf-policy/`

## 10. 后续可选增强

- 在用户界面中暴露运行时功率模式、当前功率和可选档位；
- 在遥测中增加节点当前功率和 LQI，便于界面展示；
- 将节点移动到更远/隔墙位置，验证手动固定功率可能失败、自动模式失败驱动升功率的差异；
- 优化同冷启动收敛，使节点在网关 Zigbee 就绪后再开始 rejoin；
- 多节点场景下为每个节点分别维护目标、LQI、功率和等待状态。


## 11. 2026-09-23 严格事务边界最终加固

### 修复内容

1. 将“网关命令响应”和“节点主动上报”彻底分开：低序号只可能是本次 QUERY/SET 的响应，高序号只可能是节点主动 REPORT；
2. 手动/自动模式切换必须由加密 v2 REPORT 证明；v1 REPORT 只能观测功率，不能证明模式；
3. REPORT 到达时实时判断 90 秒 deadline，避免周期任务尚未运行时迟到回包仍确认旧事务；
4. 链路陈旧超过 120 秒时仅终止用户请求，不删除 NVS 中的 durable desired mode/manual power；恢复后先 QUERY，再按结果 SET；
5. QUERY 发现 durable desired 与节点实际模式/功率不一致时自动重新对账；
6. 修复幂等固定功率场景：节点本来已是目标 `manual/power` 时也会发布 confirmed；最终确认前二次校验完整事务上下文，防止 TOCTOU 竞争。

### 最终验证

- 网关只执行 `app-flash` 到 COM6；未刷 bootloader/分区表，未擦 Zigbee NVRAM；
- 节点本轮未重新烧录，节点 app 仍为前一版本，SHA256 为 `8894C2FEA281D663873B054E535B9A6A396F7C8F8439F47D762598E09F41242F`；
- 网关重启后第一次查询时，节点仍在重新建立父链路；等待自动恢复后查询成功：`auto/-10 dBm`；
- 最终运行时序列通过：SET `0 dBm` → AUTO → SET `-10 dBm` → AUTO，最终为 `auto/-10 dBm`；
- 后端回归：`50 passed, 3 skipped`；
- 独立代码复审最终结论：P0=0、P1=0、P2=0，APPROVE。

最终网关 app SHA256：`D1EA34AD6CC0052BBEB95662B38FE79DD029963F21C36284013837D1B646B173`。

备份目录：`backups/firmware/2026-09-23-strict-rf-transaction/`。
