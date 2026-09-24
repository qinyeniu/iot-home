# 固件全链路说明：节点、网关、MQTT 与后端

更新时间：2026-09-25 02:25
状态：基于当前仓库代码与夜间长测状态梳理；本文件只做文档更新，未修改固件运行逻辑  
适用工程：

- 网关：`firmware/gateway/`
- 组合节点：`firmware/node_zigbee/`
- 共享配置组件：`firmware/common/iot_product_config/`
- 独立电池实验工程：`firmware/node_battery_sensor/`
- 后端参考：`server/backend/`

## 1. 一句话总览

当前系统是一个“单组合节点 + 单网关 + 云后端”的原型：

```text
AHT20/BH1750/按键/继电器
        ↓ I2C/GPIO
   ESP32-C6 节点
        ↓ Zigbee 802.15.4 / ZCL / 私有 RF 包
   ESP32-C6 网关
        ↓ Wi‑Fi / MQTT QoS1
 MQTT Broker / FastAPI / MySQL / Grafana
        ↓ REST / MQTT command
 用户页面或后端服务
```

节点负责采集温湿度、光照，控制同板继电器；网关既是 Zigbee 协调器，又是 Wi‑Fi/MQTT 桥接器；后端订阅 MQTT，将数据写入 MySQL，并把用户命令重新发布给网关。

## 2. 当前硬件职责

### 2.1 网关

工程：`firmware/gateway/`

角色：

- Zigbee Coordinator；
- Wi‑Fi STA；
- MQTT Client；
- SSD1306 OLED 本地状态显示；
- RF 功率策略桥接和健康自愈。

当前 I2C：

| 功能 | 引脚/地址 |
|---|---|
| OLED SDA | GPIO2 |
| OLED SCL | GPIO3 |
| SSD1306 地址 | `0x3C` |

分区：

| 分区 | 用途 |
|---|---|
| `nvs` | 普通产品配置、RF policy、网关标记、MQTT boot epoch |
| `factory` | 网关应用固件 |
| `zb_storage` | Zigbee 持久化数据 |
| `zb_fct` | Zigbee 出厂相关数据 |

### 2.2 组合节点

工程：`firmware/node_zigbee/`

角色：

- Zigbee End Device；
- AHT20 温湿度采集；
- BH1750 光照采集；
- 继电器执行器；
- 本地按键；
- RF 功率策略执行端。

当前接线：

| 功能 | 引脚/地址 |
|---|---|
| I2C SDA | GPIO2 |
| I2C SCL | GPIO3 |
| AHT20 | `0x38` |
| BH1750 | `0x23` |
| 继电器 | GPIO4，低电平吸合 |
| 本地按键 | GPIO5，低电平按下 |

## 3. 端到端数据流

### 3.1 遥测上行

```text
节点 sensor_task
  1. 读取 AHT20 和 BH1750
  2. 转换为 Zigbee 标准单位
  3. 更新本地 ZCL 属性
  4. 通过 raw APSDE-DATA 发送 ZCL Report Attributes
  5. 等待 APS 确认；失败时限次补发
        ↓
网关 ZCL core callback
  1. 校验 cluster、attribute、长度和数值
  2. 只把小型事件放入队列
        ↓
网关 zb_forward_task
  1. 更新节点表
  2. 聚合温湿度/光照
  3. 转换为云端 JSON
  4. 去重并发布 MQTT
        ↓
FastAPI MQTT 服务
  1. QoS1 订阅消息
  2. 放入内存队列
  3. worker 串行写 MySQL
  4. 事务提交后再向 Broker 返回 PUBACK
```

### 3.2 开关命令下行

```text
用户或 REST API
  ↓ MQTT topic: iot-home/{gateway}/nodes/{node}/cmd
网关 MQTT callback
  1. 复制 topic/payload
  2. 解析 JSON
  3. 识别 on/off/toggle
  4. 获取 ZBOSS lock
  5. 发送标准 ZCL On/Off 命令
        ↓
节点 zb_action_handler
  1. 收到 On/Off cluster 属性设置
  2. 驱动 GPIO4
  3. 延迟发送一次 OnOff 状态 Report
        ↓
网关
  1. 收到 OnOff report
  2. 更新节点状态
  3. 发布非保留 telemetry
```

本地按键走同一条“继电器状态变更后上报”的路径，而不是单独维护一套云端同步逻辑。

## 4. 节点固件做了什么

入口：`firmware/node_zigbee/main/main.c`

### 4.1 启动顺序

`app_main()` 的顺序：

1. **先把继电器 GPIO4 配为输出并置高**，避免启动瞬间低电平导致继电器吸合；
2. 配置按键 GPIO5 输入上拉；
3. 初始化 NVS；
4. 初始化本地 RF 功率状态；
5. 初始化 I2C 总线和 AHT20/BH1750 device handle；
6. 创建任务：
   - `rf_power_task`；
   - `sensor_task`；
   - `zigbee_task`；
   - `button_task`。

I2C 初始化失败不会阻止 Zigbee 启动；`sensor_task` 后续会尝试重建总线。

### 4.2 Zigbee End Device 配置

`zigbee_task()` 延迟 3 秒后启动协议栈：

- 设备角色：End Device；
- 限定信道：26；
- install code policy：关闭；
- keep-alive：3000 ms；
- 启动发射功率：`-10 dBm`；
- 当前代码显式设置 `rx_on_when_idle = true`。

需要特别注意：虽然结构配置里有 End Device 参数，但**当前节点为了调试保持接收窗口常开，还不是最终低功耗状态**。这也是后续电池化时必须改的核心点。

当前节点使用：

```text
esp_zb_main_loop_iteration()
vTaskDelay(100 ms)
```

网关则使用专用的 `esp_zb_stack_main_loop()`。

### 4.3 AHT20 读取

流程：

1. 发送测量触发命令：`0xAC 0x33 0x00`；
2. 等待 80 ms；
3. 读取 7 字节；
4. 检查 busy/calibrated 状态；
5. 使用 CRC-8（多项式 `0x31`）校验；
6. 根据 AHT20 公式计算温度和湿度；
7. 检查温度、湿度范围。

标准 ZCL 上报前转换为：

- 温度：`0.01 °C`，例如 `25.34 °C -> 2534`；
- 湿度：`0.01 %`，例如 `65.20 % -> 6520`。

### 4.4 BH1750 读取

流程：

1. 发送命令 `0x10`；
2. 等待 180 ms；
3. 读取 2 字节；
4. 按 `raw / 1.2` 得到 lux。

Zigbee 上报使用对数照度：

```text
measured_value = 10000 * log10(lux + 1)
```

网关恢复云端值时使用：

```text
lux = 10^(measured_value / 10000) - 1
```

这样做可以把大范围光照压缩进 16 位 Zigbee 属性，但会损失一些精度。

### 4.5 节点 Endpoint 和 Cluster

节点和网关都使用 endpoint `10`。

节点提供以下 server cluster：

| Cluster | 方向/用途 |
|---|---|
| Basic | 设备基础信息，当前 `power_source=0x03` |
| Identify | 标准识别 cluster |
| Temperature Measurement | 温度上报 |
| Relative Humidity Measurement | 湿度上报 |
| Illuminance Measurement | 光照上报 |
| On/Off | 继电器控制与状态上报 |
| 私有 `0xFC10` | RF 功率查询、设置和回报 |

### 4.6 Raw ZCL Report 的原因

节点没有使用高层 ZCL report helper 发送遥测，而是直接构造 Report Attributes 帧，通过 APSDE-DATA 发送。

原因：当前自愈逻辑需要拿到 APS user-payload 的发送确认。Raw APS 请求可以：

- 使用 APS acknowledgement；
- 使用安全发送选项；
- 注册并进入 `aps_data_confirm_cb()`；
- 区分“本地提交失败”和“已经发出但 APS 失败”。

每个属性最多本地提交重试 3 次；一批遥测最多做 2 次 APS 失败后的完整缓存补发。

## 5. 网关固件做了什么

入口：`firmware/gateway/main/main.c`

### 5.1 启动顺序

`app_main()` 的主要顺序：

1. 初始化 NVS；
2. 初始化 RF policy 和日志钩子；
3. 从 `gwzb` namespace 读取网络已形成、曾见子节点标记；
4. 启用 Wi‑Fi/IEEE 802.15.4 coexistence；
5. 初始化 I2C/OLED；
6. 加载产品运行配置：NVS 优先，编译时实验室配置回退；
7. 启动 Wi‑Fi 和 MQTT；
8. 创建 Zigbee 业务队列和诊断队列；
9. 创建任务：
   - `zigbee_task`；
   - `oled_task`；
   - `status_task`；
   - `zb_forward_task`。

当前 `ZB_JOIN_BEFORE_WIFI = 0`，即 Wi‑Fi/MQTT 与 Zigbee 并行启动；`zigbee_task` 开头仍会等待 10 秒，给 Wi‑Fi 启动留出时间。

### 5.2 Zigbee Coordinator

`zigbee_task()` 中：

- 角色：Coordinator；
- 最大子节点：10；
- install code：关闭；
- 信道：26；
- 如果没有已形成标记，fresh formation 时代码请求 PAN ID `0x2b9e`；
- 注册网关 endpoint、ZCL action handler、raw APS indication handler；
- 使用 `esp_zb_stack_main_loop()` 运行协议栈。

网络形成成功后：

- 记录 `s_network_formed = true`；
- 写 NVS namespace `gwzb` 的 `formed=1`；
- 打开网络 180 秒。

代码中的 fresh formation PAN ID 与当前开发计划里记录的实测 PAN ID `0xd989` 不一致。该事项不应靠猜测修改；硬件重新接入后应先读取当前真实 PAN，再决定是文档过时、代码改动未测，还是存在历史网络遗留。

### 5.3 为什么 Zigbee callback 不直接做复杂工作

ZBOSS 某些 callback 可能运行在不可抢占或类似 ISR 的上下文中。若在里面执行日志、newlib、MQTT 或复杂表操作，可能触发递归锁断言。

因此网关把 callback 分成两类：

1. **业务事件队列 `s_zb_app_q`**：join、report、leave；
2. **诊断信号队列 `s_zb_signal_q`**：permit join、rejoin、NLME status 等。

callback 只做校验和小对象复制，复杂处理全部交给 `zb_forward_task()`。

即使出现异常信号风暴，也优先丢弃诊断事件，不挤占 join/report/leave 的业务容量。

### 5.4 两条接收路径

网关同时注册了两类接收处理。

#### ZCL Core Action Handler

函数：`zb_action_handler()`

负责业务解析：

- 温度；
- 湿度；
- 光照；
- OnOff 状态。

它验证通过后发布 `ZB_APP_EVENT_REPORT`。

#### Raw APS Data Indication Handler

函数：`rf_power_aps_indication_cb()`

负责观察底层帧：

- 帧安全状态；
- profile/endpoint；
- LQI；
- RSSI；
- 私有 RF cluster 包；
- 节点是否为已知 active child。

标准遥测帧会同时经过业务路径和 RF 采样路径；私有功率包只由 raw APS 路径处理。

### 5.5 网关节点表

网关最多维护 10 个节点，当前以 16 位短地址作为主键。

节点表保存：

- short address；
- IEEE address；
- 温度、湿度、光照原始值；
- 数据是否有效；
- 是否有新数据待发布；
- 是否在线；
- OnOff 状态；
- 首次脏数据时间和最近上报时间。

遥测不是每收到一个属性就立刻转发，而是按节点聚合：

- 三类环境数据都到齐，形成完整样本；
- 或等待超过 30 秒仍不完整，发布部分/legacy 数据。

这样可以避免一次传感器批次产生三条 MQTT 消息，也兼容未来 sleepy node 的属性延迟到达。

### 5.6 MQTT Topic

当前网关硬编码前缀：

```text
iot-home/gw-001
```

发布的主题：

| Topic | QoS | Retain | 内容 |
|---|---:|---:|---|
| `nodes/zb-XXXX/telemetry` | 1 | 否 | 传感器和开关普通事件 |
| `nodes/zb-XXXX/status` | 1 | 是 | 节点在线/离线状态 |
| `nodes/zb-XXXX/rf_policy` | 1 | 是 | RF 模式、功率、命令状态 |
| `nodes/gw-001/status` | 1 | 是 | 网关在线状态、心跳、LWT |
| `nodes/gw-001/telemetry` | 1 | 否 | 网关心跳遥测 |

订阅：

```text
iot-home/gw-001/nodes/+/cmd
```

### 5.7 MQTT message_id

非保留消息通过 `mqtt_pub()` 发送时，网关会尽量在 JSON 开头加入：

```json
{"message_id":"...","data":{}}
```

message_id 包含：

- Wi‑Fi MAC；
- boot epoch；
- tick 时间；
- 本 boot 内递增序号。

boot epoch 存放在 NVS namespace `gwmqtt`，重启后递增，避免不同启动轮次产生相同编号。NVS 不可用时退回硬件随机数方案，但不会阻塞遥测。

后端可用 message_id 做幂等处理。

### 5.8 RAM Outbox

当前非保留 MQTT 消息有一个 RAM FIFO：

- 容量：30；
- topic suffix：64 字节；
- payload：192 字节；
- 队列满时丢弃最老消息；
- MQTT 恢复后自动补发。

这是短断网的尽力缓存，不写入 Flash，适合当前“减少 Flash 磨损”的要求。

保留状态不进入 outbox；每次 MQTT 重连后网关会执行 `status_resync`，重新发布网关和在线节点状态。

### 5.9 OLED 显示

OLED 使用本地 SSD1306 文本驱动，而不是依赖第三方文字 glyph 路径。

显示内容包括：

- Wi‑Fi/MQTT 状态；
- IP；
- Zigbee 节点数；
- 启动和运行提示。

OLED 不可用时网关继续运行，不影响主链路。

### 5.10 健康重启策略

网关通过日志钩子统计：

```text
ZB_ESP_MAC: No buffer, handle it
```

只有同时满足以下条件才受控重启：

1. 180 秒没有收到有效子节点帧；
2. MAC no-buffer 事件至少 100 个；
3. 事件达到约 60 秒持续/密集条件；
4. 最近 5 秒内事件仍新鲜。

普通节点正常拔掉时，不会满足 MAC no-buffer 风暴条件，因此网关不应重启；节点状态会更新为离线。

## 6. 私有 RF 功率协议

私有 cluster：

```text
0xFC10
```

当前 v2 包长 8 字节：

| 字段 | 内容 |
|---|---|
| magic0 | `0x52`，`R` |
| magic1 | `0x46` | `F` |
| version | `2` |
| command | `QUERY=1` / `SET=2` / `REPORT=3` |
| sequence | 包序号 |
| power | int8，单位 dBm |
| mode | `AUTO=0` / `MANUAL=1` |
| reserved | `0` |

### 6.1 序号边界

- 网关发起的命令及期待回报使用低序号 `1..127`；
- 节点主动回报使用 `0x80..0xFF`；
- confirmed report 必须同时匹配本次 request sequence、功率和模式；
- 用户 `request_id` 只允许字母、数字、下划线和中划线；
- 超过等待期限后，即使收到数值正确的旧 REPORT，也不能确认原事务。

### 6.2 自动功率策略

当前原则：

- 不根据单个较弱 LQI 直接升功率；
- 真实 commissioning 失败或 APS 失败达到阈值才升高；
- LQI 持续很好时，可在 AUTO 模式下降低不必要的高功率；
- 节点启动总是先回到经过近距验证的 `-10 dBm`，避免把旧位置学习到的高功率带回网关旁边；
- RF policy 的 desired mode/manual power 持久化在 NVS namespace `rfpol`。

## 7. 后端如何接全链路

后端 MQTT 服务订阅：

```text
iot-home/+/nodes/+/telemetry
iot-home/+/nodes/+/status
iot-home/+/nodes/+/rf_policy
```

处理策略：

1. QoS1 订阅；
2. 网络循环只快速收消息进内存队列；
3. worker 串行处理，保持同一设备顺序；
4. 数据库事务成功后才手动返回 PUBACK；
5. 瞬时数据库错误有限重试；
6. 重试失败写入 dead letter；
7. 关停时先排空队列。

后端 REST 命令发布到：

```text
iot-home/{gateway_id}/nodes/{node_id}/cmd
```

当前网关固件仍使用实验室硬编码 ID：`gw-001` 和 `zb-XXXX`。产品化时需要统一“固件使用的逻辑 ID”和“后端设备 ID”生成/校验规则。

## 8. 当前实现的主要改进点

### 8.1 先解决，不建议绕过

1. **NVS A/B Bundle 需硬件验证**  
   多个 legacy 字符串键不能靠一次 `nvs_commit()` 形成事务；当前已离线实现 `cfg_a/cfg_b/cfg_commit` A/B 接口。SoftAP `/commit` 前仍需在真实网关上验证 save/load/restart、掉电恢复和旧配置回退，当前运行时尚未调用该接口。

2. **节点身份稳定性**  
   短地址可能变化，不能作为长期产品 ID。建议以 IEEE address/安装阶段生成的稳定 node ID 做映射，短地址只作为当前路由地址。

3. **命令目标校验**  
   on/off 命令路径当前主要按 topic 中的短地址转发，应在发送前校验该短地址属于 active child，并确认其具备 OnOff 能力，避免向不存在或非执行器地址发命令。

4. **PAN ID 事实核对**  
   代码请求值和当前计划记录的实测值不一致。硬件在线时先读取真实网络参数，再更新文档或调整代码。

### 8.2 产品化阶段处理

1. SoftAP + 本地网页配网；
2. 凭据保存与 NVS 加密路线评估；
3. 低磨损 Flash spool，仅在明确需要且使用专用分区时实施；
4. 电池节点独立工程：已离线完成 SED、60 秒采集上报、按需 I2C；下一步硬件实测电流，再决定负载开关或 deep sleep；
5. deferred command 和电池节点“不保证即时到达”的前后端状态；
6. TLS/WSS、MQTT ACL、VPN 或安全组白名单；
7. 后端积压、dead letter、磁盘和数据库资源监控。

### 8.3 代码结构建议

当前两个 `main.c` 都很大，网关文件约 3500 行，节点文件约 2600 行。等关键行为有稳定测试后，建议逐步拆为：

- sensor 模块；
- relay/button 模块；
- Zigbee endpoint/cluster 模块；
- RF policy 模块；
- MQTT outbox/publish 模块；
- node registry 模块；
- OLED 模块；
- provisioning 模块。

拆分必须小步进行，每一步保持构建和测试通过；不应在没有硬件验证计划时大规模重写。

## 9. 本次没有做的事

- 未修改固件运行逻辑；
- 现网短序列和拔节点测试已完成，8 小时长测仍在进行；
- 电池实验工程与新 NVS 接口未烧录/未接入运行时；
- 未修改网卡、热点、安全组或云服务器；
- 未读取、打印或记录真实密码文件；
- 未修改分区表；
- 未清除 Zigbee NVRAM；
- 未启用 NVS encryption、Secure Boot 或 Flash encryption。
