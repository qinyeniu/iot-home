# IoT-Home Zigbee 低功耗终端设计方案

更新时间：2026-09-26
状态：独立电池节点方案；现网 COM5 组合节点不替换、不烧录此固件
对应固件：`firmware/node_battery_sensor`

## 1. 最新结论

第一版电池节点不采用“运行中直接进入 deep sleep”的方案，而采用：

```text
Zigbee Sleepy End Device（SED）
  + ZBOSS 原生休眠
  + ESP-IDF PM light sleep / tickless idle
  + 传感器和 I2C 按需初始化、采样后释放
```

原因：Zigbee 协议栈运行时直接 deep sleep 会破坏 RAM/网络连接状态，唤醒后需要重新 join/rejoin，射频能耗和恢复时间都不可忽视。先让 ZBOSS 自己在空闲时进入休眠，更适合作为产品化第一阶段。

## 2. 当前电池固件已经具备的能力

当前独立固件 `firmware/node_battery_sensor`：

1. 以 Zigbee Sleepy End Device 身份加入；
2. 开启 ZBOSS sleep：
   - `esp_zb_sleep_enable(true)`；
   - `esp_zb_sleep_set_threshold(500 ms)`；
   - `esp_zb_set_rx_on_when_idle(false)`；
3. 开启 ESP-IDF 电源管理：
   - `CONFIG_PM_ENABLE=y`；
   - `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`；
   - `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP=y`；
   - `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH=y`；
4. Zigbee 入网后才初始化 I2C；
5. 每轮采样 AHT20 + BH1750，采样后删除 I2C device、释放 I2C bus；
6. 使用标准 ZCL 属性上报温湿度、湿度、光照；
7. 本地 APS 请求被拒绝时最多尝试 3 次，帧已接受后的失败等待下一采样周期，不形成射频风暴；
8. 默认启动发射功率为 `-10 dBm`，避免启动时无条件高功率。

当前调试参数：

| 参数 | 当前值 | 产品化建议 |
|---|---:|---|
| 采样/上报周期 | 60 秒 | 300–1200 秒，按数据需要调整 |
| keep-alive/poll | 60 秒 | 300 秒左右，按下行命令延迟要求调整 |
| 本地提交尝试 | 3 次 | 保持 2–3 次 |
| 启动发射功率 | -10 dBm | 保持保守启动，再由 RF policy 调整 |

## 3. 典型一次工作周期

```text
空闲 light sleep
  → keep-alive/poll 时间到，短暂唤醒与父节点同步
  → 到达采样周期
  → 初始化 I2C bus
  → 添加 AHT20/BH1750 device
  → AHT20 触发、等待约 80 ms、读取并校验 CRC
  → BH1750 one-shot、等待约 180 ms、读取
  → 依次提交 ZCL 属性上报
  → 删除 I2C device / bus
  → 回到 ZBOSS + light sleep
```

## 4. 功耗测量计划

必须在独立电池实验板上测量，不能把电池固件烧入当前 COM5 组合节点。

### 4.1 测量状态

分别记录：

1. USB 断开、电池供电时的 idle/light sleep；
2. Zigbee keep-alive/poll；
3. I2C 初始化；
4. AHT20/BH1750 测量；
5. 单属性 ZCL report；
6. 完整一次采样到恢复休眠；
7. 连续 10–24 小时平均电流。

每次记录电压、电流峰值、持续时间和对应日志时间，方便把电流波形和固件日志对齐。

### 4.2 重点漏电来源

| 漏电来源 | 处理方式 |
|---|---|
| 板载电源指示灯/用户 LED | 产品板移除；实验板确认是否可切断 |
| USB-Serial/JTAG 芯片 | 电池测试时断开 USB；产品板增加隔离或不焊 |
| I2C 上拉 | 使用高阻上拉；必要时通过负载开关关闭 |
| AHT20/BH1750 待机电流 | 使用负载开关给传感器供电 |
| 传感器电源与主控电源共轨 | PCB 分电源域，采样后关闭传感器电源 |
| 分压式电池电压检测 | 使用高阻分压，采样时再通过 MOSFET 接通 |
| Zigbee 反复 poll | 拉长 keep-alive；权衡下行命令延迟 |

## 5. 固件后续优化顺序

### P0：实测基线

- 构建当前电池固件；
- 使用独立板入网并跑至少 10 个周期；
- 获取真实平均电流，不凭开发板标称值判断。

### P1：减少上报次数

当前温湿度、湿度、光照分三条 ZCL report，射频事务较多。后续应改为**一条多属性 ZCL report**：

```text
一个 ZCL frame 内包含 temperature + humidity + illuminance
```

目标：减少 APS 请求、CPU 唤醒和射频发射次数。改动前必须确认网关现有 ZCL report 解析器支持同一帧多个属性，并用主机定向测试覆盖。

### P2：硬件电源域

- 给 AHT20/BH1750 和 I2C 上拉增加负载开关；
- 采样前上电、采样后断电；
- 主控保持 SED 休眠；
- 测量并确认传感器断电后 I2C bus 不会悬空。

### P3：电池电压和电量估算

- 增加电池电压采样；
- 只在采样周期读取，避免常驻分压耗电；
- 电池电量模型按化学体系区分：碱性、Li-ion、LiFePO4；
- 低电量作为普通遥测上报，不高频报警。

### P4：再评估 deep sleep duty cycle

只有在以下条件同时成立时才考虑 deep sleep：

- 采样间隔很长（例如 10 分钟以上）；
- 下行控制延迟可以接受；
- SED light sleep 电流仍达不到目标；
- secure rejoin 的能耗和时间已实测可接受。

deep sleep 版本的唤醒流程应固定为：

```text
唤醒 → 初始化最小外设 → secure rejoin/poll → 采样上报 → 处理待办 → deep sleep
```

## 6. 电池和硬件建议

- CR2032 不适合作为 Wi‑Fi/Zigbee 脉冲发射的主电源，内阻偏高；
- 2 节 AAA/AA 可用于低平均电流实验，但需确认 LDO 最低输入和脉冲压降；
- 18650/小型 Li-ion 配合低静态电流稳压更适合原型；
- PCB 上预留电流表跳线/0Ω 电阻，方便量产测试；
- 电池节点外壳设计前必须确定电池尺寸、充电方式和电源开关位置。

## 7. 云端和网关约束

- 电池节点不直连云端，不轮询服务器；
- 下行命令由网关和 MQTT 接收，再通过 Zigbee 间接下发；
- 服务器不能对电池节点做高频重试，避免设备反复唤醒；
- 后端、MQTT、MySQL 和日志都要保留资源余量；
- 当前不启用 TLS/WSS/VPN，也不修改阿里云 ECS；后续由用户在场时教学式配置。

## 8. 不自动执行的事项

- 不烧录 COM5 现网组合节点；
- 不修改前端/后端生产代码；
- 不部署远程服务器；
- 不刷 bootloader/分区表，不擦 Zigbee NVRAM；
- 不启用 Secure Boot、Flash encryption 或 NVS encryption；
- 不在没有电流实测前承诺电池续航。

