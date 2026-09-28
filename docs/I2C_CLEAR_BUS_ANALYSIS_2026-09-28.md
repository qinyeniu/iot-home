# 网关 I2C `clear bus error` 只读预分析

- 分析时间：2026-09-28 22:20（8 小时长测进行中）
- 对象：COM6 网关默认 OFF 固件（`B14D6C37...`）
- 状态：**预分析 / 未改动任何固件与配置**

## 1. 现象

长测窗口内反复出现 ESP-IDF I2C 驱动错误：

```text
E (xxxxx) i2c: clear bus error
```

截至 uptime≈1746 秒共 **9 次**，发生时刻（ms）：

```text
22401, 145971, 302961, 579841, 610361, 1070361, 1272091, 1484701, 1746341
```

相邻间隔（秒）：

```text
123.6, 157.0, 276.9, 30.5, 460.0, 201.7, 212.6, 261.6
```

间隔无固定周期/倍数规律，属**概率性事件**，不是某固定软件周期直接触发。

## 2. 触发机制（ESP-IDF v5.5.4 旧版 I2C 驱动）

ESP32-C6 定义 `SOC_I2C_SUPPORT_HW_CLR_BUS`，使用硬件 clear bus 状态机：

1. `i2c_master_cmd_begin()` 在总线异常时调用 `i2c_hw_fsm_reset()`：
   - 进入时 `status==I2C_STATUS_TIMEOUT` 或检测到 bus busy；
   - DONE 事件但 status=TIMEOUT（从机掉电/SDA-SCL 被拉地）；
   - ACK_ERROR 累计达到 `I2C_ACKERR_CNT_MAX`；
   - 等待事件队列超时（无中断、总线疑似死亡）。
2. `i2c_hw_fsm_reset()` 复位 FSM 后调用 `i2c_master_clear_bus()`。
3. 硬件状态机产生最多 9 个 SCL 脉冲尝试让从机释放 SDA；
   若 **50 ms 内未完成**（100 kHz 下 9 脉冲仅需约 90 μs），
   打印 `clear bus error`，返回 `ESP_ERR_INVALID_STATE`。

50 ms 超时说明 **SDA 被外部（从机）持续强拉低，9 个时钟内未释放**，
是典型的“I2C 从机异常锁死总线 / 接线接触”特征，而非协议时序软件 bug。

## 3. SSD1306 组件的恢复机制

`firmware/common/iot_ssd1306/iot_ssd1306.c` 已具备两级恢复：

1. 单帧 `oled_flush_once()` 内重试 3 次，每次重试重置窗口；
2. 整帧失败后 `iot_ssd1306_recover()`：
   `i2c_driver_delete` → 80 ms → GPIO bit-bang 9 脉冲 + STOP
   （`oled_i2c_force_bus_idle`）→ 重建驱动 → probe → 面板重新初始化。

日志佐证：9 次 clear bus 中仅 71 秒那 1 次走到完整
`OLED refresh failed` 恢复并成功；其余 8 次在帧内重试或下一帧即恢复，
之后每秒 flush 持续成功。

## 4. 实际影响评估

- 每次事件后 1～4 秒内业务行（RF sample / cached report / MQTT）立即恢复；
- Zigbee 采样、缓存、MQTT 遥测全程连续，未见连锁失败；
- 显示基本正常，仅一次需要本地完整恢复；
- 但按既定长测判定口径，`clear bus error` 属于 critical 文本，
  **长测最终将判为 FAIL/WARN**，即使业务连续性未受破坏。

## 5. 与 Wi-Fi 发射的相关性排查

每次事件距上一条 MQTT telemetry 的间隔约 1.2～9.6 秒，
而 telemetry 周期约 10.6 秒；该分布近似 0～10.6 秒均匀分布，
**不支持“Wi-Fi 发射电源噪声直接触发”的因果关系**。
根因更可能在 OLED 模块/接线本身的概率性锁死。

## 6. 根因假设（按可能性排序）

1. SSD1306 模块在长时间运行、供电微扰、热循环下偶发进入异常状态拉住 SDA；
2. 杜邦线/面包板 SDA 接触电阻变化或与邻近线偶发短路、外部上拉不足；
3. OLED 供电去耦不足（模块 VCC 缺少 100 nF 去耦）。

311 秒短测未触发，长测窗口拉长后出现，符合低概率硬件事件特征。

## 7. 建议（不在长测期间实施）

硬件（优先、成本最低）：

1. 检查并固定 SDA/SCL 杜邦线，尽量缩短排线；
2. SDA/SCL 增加外部 4.7 kΩ 上拉；
3. OLED VCC 就近加 100 nF 去耦电容。

固件（待硬件排查后再评估，避免在 54 KB 紧张空间内盲目加逻辑）：

1. 保持现有两级恢复；
2. 可统计“clear bus 后帧内恢复 vs 完整 recover”的分项计数，
   帮助区分瞬时锁死与持续性故障；
3. 若硬件整改后仍高频出现，再评估降低 I2C 速率或调整 flush 周期。

## 8. 对后续工作的影响

- 长测分析将把 9+ 次 clear bus 计为 critical，结论倾向 FAIL/WARN；
- 按规则：长测 FAIL 时**先定位、不覆盖固件**；
- Zigbee 事件队列拆分（P2-1～P2-4）暂缓，先完成本项排障闭环。
