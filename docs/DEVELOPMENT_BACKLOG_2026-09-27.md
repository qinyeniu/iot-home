# 后续开发计划单与 ESP32-C6 资源快照

日期：2026-09-27  
基线提交：`93ac31c 记录夜间长测最终结果`  
固件：网关默认 OFF 构建  
依据：`backups/long-test-20260927-default-4h/analysis.md`

## 1. 当前资源快照

### 1.1 板卡与系统配置

```text
芯片：ESP32-C6
CPU：单核 RISC-V，160 MHz
Flash：板载 4 MB
PSRAM：无
控制台：USB Serial/JTAG
网络：2.4 GHz Wi‑Fi + 802.15.4/Zigbee 共存
```

### 1.2 Flash 分区

| 分区 | 起始地址 | 大小 | 用途 |
| --- | ---: | ---: | --- |
| nvs | `0x09000` | 24 KiB | 普通配置 |
| phy_init | `0x0f000` | 4 KiB | PHY 初始化数据 |
| factory | `0x10000` | 1472 KiB | 网关应用固件 |
| zb_storage | `0x180000` | 16 KiB | Zigbee 持久数据 |
| zb_fct | `0x184000` | 4 KiB | Zigbee 工厂数据 |

当前已规划分区结束于 `0x185000`：

```text
已规划：1556 KiB，占 4 MB Flash 的 38.0%
分区后未分配：2540 KiB，占 62.0%
```

### 1.3 应用固件占用

```text
固件文件：firmware/gateway/build/iot-home-gateway.bin
bin 大小：1445616 bytes / 1411.7 KiB
应用分区：1507328 bytes / 1472.0 KiB
分区剩余：61712 bytes / 60.3 KiB
应用分区使用率：95.9%
```

Flash 中段占用：

```text
Flash code/text：1123818 bytes / 1097.5 KiB
Flash rodata：171432 bytes / 167.4 KiB
Flash 段合计：1295506 bytes / 1265.1 KiB
```

含义：

- 当前能继续加小功能；
- 60 KiB 左右空间可以容纳小规模状态观测、轻量协议逻辑和 gzip 后的小型配网页；
- 不适合在当前应用分区内直接加入较大的 Web 资源、完整 OTA 双分区或大量证书；
- Flash 芯片实际还有约 2.48 MiB 未规划空间，后续确有必要时可扩大应用分区，但这需要更新分区表，必须单独安排维护窗口，不能在日常开发中顺手修改。

### 1.4 静态 RAM 占用

```text
静态 D/IRAM 已用：202570 bytes / 197.8 KiB
静态 D/IRAM 总量：452112 bytes / 441.5 KiB
链接后剩余：249542 bytes / 243.7 KiB
使用率：44.8%
```

组成：

```text
.data：16604 bytes / 16.2 KiB
.bss：52704 bytes / 51.5 KiB
IRAM-resident text：133262 bytes / 130.1 KiB
```

应用任务栈：

| 任务 | 栈大小 |
| --- | ---: |
| zigbee | 16384 bytes |
| WiFi reconnect | 5120 bytes |
| oled | 4096 bytes |
| status | 4096 bytes |
| zb-fwd | 4096 bytes |
| 合计 | 33792 bytes / 33.0 KiB |

说明：上面的“链接后剩余”不是运行时空闲堆。任务栈、Wi‑Fi/Zigbee 运行时缓冲会在启动后动态占用一部分。当前固件还没有持续上报 `free heap / minimum free heap`，因此运行时剩余堆需要在“资源观测”任务加上后才能给出精确实测值。

### 1.5 主要 Flash 占用来源

| 模块 | 大致 Flash 占用 | 说明 |
| --- | ---: | --- |
| ZBOSS stack | 307.8 KiB | Zigbee 协议栈 |
| 802.11 MAC | 184.5 KiB | Wi‑Fi MAC 层 |
| lwIP | 121.3 KiB | TCP/IP 协议栈 |
| pp | 109.6 KiB | 无线基带相关 |
| mbedcrypto | 99.6 KiB | 加密算法 |
| wpa_supplicant | 78.8 KiB | Wi‑Fi 认证 |
| MQTT | 18.2 KiB | MQTT 客户端 |
| 应用 main | 26.6 KiB | 当前网关应用代码 |

RAM 方面最明显的是 Zigbee、Wi‑Fi MAC 和 lwIP 的静态缓冲；当前仍有余量，但后续应持续监控堆的最低水位。

## 2. 开发优先级总览

| 优先级 | 任务 | 是否需要硬件 | 建议顺序 |
| --- | --- | --- | ---: |
| P0 | 加入运行时资源观测 | 短测需要 | 1 |
| P0 | 分析并治理 Zigbee 父链路抖动 | 需要 | 2 |
| P1 | 拆分 MQTT 客户端服务 | 短测需要 | 3 |
| P1 | 拆分 RF 私有协议编解码与功率策略 | 短测需要 | 4 |
| P1 | 拆分 Wi‑Fi 恢复策略 | 短测需要 | 5 |
| P1 | 拆分 Zigbee 应用事件和节点模型 | 需要 | 6 |
| P2 | 集成 SoftAP 配网与 NVS A/B 配置 | 维护窗口 | 7 |
| P2 | MQTT ACL/TLS 安全加固 | 维护窗口 | 8 |
| P3 | 电池节点低功耗实测 | 需要独立板/电流表 | 9 |
| P3 | 多节点、Web 控制、远距离和产品化 | 需要 | 10 |

## 3. P0：先补观测和稳定性

### P0-1 运行时资源观测

目标：在不改变业务路径的前提下，获得真实运行资源数据。

计划加入：

- 当前空闲堆 `esp_get_free_heap_size()`；
- 历史最低空闲堆 `esp_get_minimum_free_heap_size()`；
- 最大连续空闲块；
- 复位原因；
- 关键任务栈高水位：zigbee、WiFi reconnect、oled、status、zb-fwd；
- Wi‑Fi 重连、beacon timeout、MQTT 断连、RF 失败、OLED 恢复次数；
- 30 秒心跳中携带资源字段，或在 OLED 上增加一个诊断页。

验收标准：

1. 默认 OFF 构建通过；
2. 硬件短测至少 10 分钟；
3. 日志能看到准确的 free heap/min heap；
4. 连续运行期间最低堆不持续下降；
5. OLED 正常刷新，业务遥测不中断。

### P0-2 Zigbee 父链路抖动治理

昨晚长测中，01:48:33 至 02:15:18 出现 parent unavailable 事件：

```text
unavail：+142
active probes：+133
resteer：+132
aps_fail：+2
最终自动恢复：+1
```

虽然没有形成超过 300 秒的业务黑障，但持续约 27 分钟的事件抖动需要单独处理。

计划：

1. 对齐节点和网关时间线，确认事件来源；
2. 判断是真实父链路故障、poll/APS 超时，还是应用层检测过于敏感；
3. 给 parent unavailable 检测增加连续次数确认和冷却时间；
4. 避免在短时间内反复 resteer；
5. 保留最终无法恢复时的强制 steering；
6. 增加更清晰的日志：首次异常、确认异常、恢复、抑制次数。

验收标准：

- 人为制造短暂父链路波动时不误判；
- 真实断链时仍能自动恢复；
- 不应出现长时间反复 probe/resteer；
- 业务遥测继续保持连续。

### P0-3 OLED I2C clear bus 观察

网关出现 15 次 `i2c: clear bus error`、2 次 OLED local recovery，均自动恢复。

建议作为独立小任务：

- 先检查屏幕排线、上拉电阻和连接可靠性；
- 记录错误发生间隔；
- 评估是否降低 I2C 频率或增强总线恢复；
- 不和 Zigbee 父链路治理放在同一次修改中。

## 4. P1：继续基础驱动和服务拆分

### P1-1 拆分 MQTT 客户端服务

新增建议组件：

```text
firmware/common/iot_mqtt_client/
```

职责：

- MQTT 客户端初始化、启动、停止；
- 连接状态；
- 普通发布、retained 发布；
- 订阅与事件回调；
- 自动重连状态暴露。

第一阶段不搬 outbox 和业务主题拼装，只拆客户端生命周期，控制风险。

验收：

- 冷启动后 MQTT connected；
- 发布、订阅、状态心跳正常；
- 断网后恢复路径与当前版本一致；
- `main.c` 行数继续下降。

### P1-2 拆分 RF 私有协议和功率策略

建议顺序：

1. 先拆纯逻辑的 packet codec：`iot_rf_packet`；
2. 再拆功率策略服务：`iot_rf_power_policy`；
3. 最后整理命令队列和后台控制器目录。

第一步应做到输入输出可在主机测试，不依赖 ESP-IDF 和 Zigbee。

### P1-3 拆分 Wi‑Fi 恢复策略

将以下内容从 `main.c` 抽为可替换策略：

- RF health scan；
- boot watchdog；
- 目标 SSID 可见性判断；
- 连续空扫描后的射频强复位。

该任务与 provisioning owner gate 有耦合，应等 MQTT 服务拆分完成后再做。

### P1-4 拆分 Zigbee 应用层

最终再拆：

- Zigbee 节点表；
- join/leave/report 事件队列；
- 节点在线状态；
- Zigbee → MQTT/OLED 的应用联动。

这是较大的一块，不建议在没有前面服务边界时直接搬迁。

### P1-5 清理历史备份文件

`firmware/gateway/main/` 中仍有：

```text
main_backup.c
main_v2.c
main.c.20260910-oled-debug.bak
```

建议确认未参与编译后，移动到仓库内历史归档目录，避免后续误打开或误修改。不做无确认的永久删除。

## 5. P2：产品配网和安全

### P2-1 SoftAP 配网分阶段集成

已有离线资产：配网会话、HTTP 适配器、命令队列、worker、owner、phase、恢复静默协调器。

建议阶段：

1. 只启动最小 HTTP status，不改变现有 Wi‑Fi；
2. 接入 owner gate；
3. 支持临时 Wi‑Fi test；
4. 验证恢复原网络；
5. 最后接入 commit。

每个阶段都要单独构建、短测和提交。由于应用分区只剩约 60 KiB，加入 HTTP 服务后必须立即重新评估 Flash 余量。

### P2-2 NVS A/B Bundle 硬件验证

需要用户在场：

- save → load → restart → load；
- 写失败后旧配置仍可用；
- marker/CRC 异常时回退；
- 模拟写入中断；
- 启动优先级：bundle > legacy keys > build default。

### P2-3 MQTT 安全加固

顺序：

1. 关闭匿名访问；
2. 新旧账号短期共存；
3. 配置 ACL；
4. 验证设备和后端；
5. 移除旧账号；
6. TLS/WSS/VPN 后续单独教学式配置。

## 6. P3：电池节点与产品功能

1. 使用独立实验板烧录电池节点固件，不替换现网 COM5；
2. 测量 sleep、poll、report、传感器采样电流；
3. 根据漏电来源决定负载开关、poll 周期或 deep sleep；
4. 支持多节点能力模型和短地址管理；
5. Web 控制界面加入认证、权限和安全传输；
6. 隔墙/远距离 ATPC 实测；
7. 最后进入 PCB、外壳和量产化设计。

## 7. 建议立即开始的任务

建议下一步先做 **P0-1 运行时资源观测**：

- 修改范围小；
- 不改变核心联网状态机；
- 能马上获得真实 free heap/min heap；
- 后续每个驱动和服务拆分都能利用这些数据判断是否引入内存泄漏或栈风险。

完成并短测通过后，再进入 **P1-1 MQTT 客户端服务拆分**。

## 8. 执行规则

- 每次只做一个清晰阶段；
- 默认 OFF 构建通过后再硬件短测；
- 不使用 `git add -A`；
- 不 push、不部署；
- 不擦 Flash/NVRAM，不刷 bootloader/分区表，除非用户明确要求；
- 每阶段完成后更新交接文档和 Git。

## 9. 10:24 执行状态更新

1. P0-1 网关运行时资源观测已完成并提交：`01b4ce6 加入网关运行时资源观测`。
2. P1-1 MQTT 客户端服务拆分已完成，最终 5 分钟硬件短测通过，随下一提交入库。
3. 当前默认 OFF 固件：`1448784 bytes`，应用分区剩余 `58544 bytes`；实验 ON 固件：`1466352 bytes`，应用分区剩余 `40976 bytes`。
4. 后续选择：若继续驱动复用，先做 P1-2 RF packet 纯逻辑编解码；若优先稳定性，先做 P0-2 Zigbee 父链路抖动治理。
## 10. 10:49 P1-2 第一步：RF packet codec 已完成

1. 新增纯逻辑组件 `firmware/common/iot_rf_packet/`，集中私有 RF 包的 magic、version、command、sequence、signed power、V2 mode 和 reserved 校验。
2. 组件不依赖 ESP-IDF、Zigbee、FreeRTOS 或硬件；已提供 `firmware/common/iot_rf_packet/README.md` 和主机侧 CTest。
3. 主机测试通过，覆盖 V1/V2 round-trip、负功率、版本/长度不匹配、非法命令/模式/保留字节、空指针和输出缓冲不足。
4. 默认 OFF 固件构建通过：`1449168 bytes`，应用分区剩余 `58160 bytes`；实验 ON 固件构建通过：`1466736 bytes`，应用分区剩余 `40592 bytes`。
5. 10:42:34–10:47:30 硬件 5 分钟短测：节点 DATA/REPORT_SEND 各 28；网关 RF sample 85、cached report 84、MQTT telemetry 28；1 次私有 QUERY 成功并被节点 REPORT 接受。
6. 无 Wi‑Fi/MQTT 掉线、MQTT 写入超时、重启、panic、watchdog、`ESP_ERR_NO_MEM`；节点传感器 I2C 失败为 0。
7. 网关出现 1 次 `i2c: clear bus error`，未触发 OLED 本地恢复；继续作为既有偶发观察项，不构成本次回归。
8. 代码复审 APPROVE，Critical/High/Medium/Low 均为 0。
9. P1-2 后续步骤：在 packet codec 稳定后，再拆分 `iot_rf_power_policy`，本阶段不搬功率策略和命令队列。

## 11. 13:22 P1-2 第二步：自动功率策略组件已完成

1. 新增纯逻辑组件 `firmware/common/iot_rf_power_policy/`，集中功率表、允许范围、LQI 阈值、样本门槛、good window 迟滞和降级目标决策。
2. 组件不依赖 ESP-IDF、Zigbee、FreeRTOS 或硬件；已提供 README 与主机 CTest。
3. 主机测试通过，覆盖阈值相等、样本不足、越界、连续 3 个好窗口、最低功率限制、计数器饱和和空参数。
4. 默认 OFF 固件构建通过：`1449664 bytes`，应用分区剩余 `57664 bytes`；实验 ON 固件构建通过：`1467056 bytes`，应用分区剩余 `40272 bytes`。
5. 13:16–13:21 硬件 5 分钟短测：节点 DATA 29、REPORT_SEND 28；网关 RF sample 84、cached report 83、MQTT telemetry 27；启动 QUERY 被接受，随后 4 次自动功率 keep。
6. 无 Wi‑Fi/MQTT 掉线、MQTT 写入超时、重启、panic、watchdog、`ESP_ERR_NO_MEM`；节点传感器 I2C 失败为 0。
7. 启动初期 OLED 出现 1 次刷新失败，随后本地 I2C/SSD1306 reset 自动恢复；未出现 `i2c: clear bus error`。
8. 代码复审 APPROVE，Critical/High/Medium/Low 均为 0。
9. P1-2 后续：packet codec 和 auto policy 已独立；下一块如需继续，可整理事务等待/QUERY 协调逻辑，但应单独设边界。
## 12. 14:22 P1-2 第三步：RF transaction 组件已完成

1. 新增纯逻辑组件 `firmware/common/iot_rf_transaction/`，集中链路过期、QUERY、等待 REPORT、超时重查、周期 reconcile 和 REPORT 字段匹配。
2. 组件不依赖 ESP-IDF、Zigbee、FreeRTOS 或硬件；已提供 README 与主机 CTest，主机测试通过。
3. 默认 OFF 固件构建通过：`1449904 bytes`，应用分区剩余 `57424 bytes`；实验 ON 固件构建通过：`1467200 bytes`，应用分区剩余 `40128 bytes`。
4. COM6 已执行应用区短刷，未擦 Flash/NVRAM，未刷 bootloader/分区表。
5. 第一轮 5 分钟短测启动 QUERY/REPORT 正常，但出现一次约 28 秒 MQTT 写入超时/断连并自动恢复；追加复测。
6. 第二轮 5 分钟复测实际触发 periodic reconcile：QUERY seq=3、APS confirm、REPORT accepted 正常；节点 DATA/REPORT_SEND 各 28，网关 RF sample 85、cached report 83、MQTT telemetry 27。
7. 第二轮无 MQTT 掉线、写入超时、复位、panic、watchdog、`ESP_ERR_NO_MEM`；节点 I2C 失败为 0。
8. I2C clear bus 两轮共 3 次，但没有 OLED 刷新失败；继续作为既有硬件/总线观察项。
9. 代码复审 APPROVE，Critical/High/Medium/Low 均为 0。
10. P1-2 当前已形成 packet codec、power policy、transaction 三个可复用纯逻辑组件；下一步可安排本版本长测，或继续拆 Zigbee 应用事件/节点模型。
