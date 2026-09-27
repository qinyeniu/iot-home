# iot_rf_power_policy

自动 RF 发射功率策略的纯逻辑组件。组件不依赖 ESP-IDF、FreeRTOS、Zigbee 或硬件，可在电脑主机上直接编译测试。

当前阶段只包含现有默认固件中的自动降功率迟滞决策；发送 Zigbee 私有包、等待节点确认、事务超时和手动模式仍由网关应用负责。

## 配置

`iot_rf_power_policy_config_t` 描述功率表和策略参数：

- `levels_dbm`：严格递增的功率表；
- `min_power_dbm` / `max_power_dbm`：允许范围，必须精确存在于功率表；
- `lqi_good_threshold`：LQI EMA 严格大于该值才计为一个好窗口；
- `good_windows_needed`：连续好窗口达到该数量后允许降低一级；
- `samples_needed`：评估前要求的链路样本数量。

当前网关配置：

```text
levels = -10, 0, 8, 14, 18, 20 dBm
range  = -10..20 dBm
LQI good threshold = 240（240 本身不算好）
good windows needed = 3
samples needed = 6
```

## 决策规则

一次评估由 `iot_rf_power_policy_decide_auto()` 完成：

1. 样本数不足时不改变好窗口计数；
2. 当前功率超出 min/max 范围时不改变计数；
3. LQI EMA 严格大于阈值时，good window 加一并在 `uint8_t` 最大值处饱和；
4. LQI EMA 小于或等于阈值时，good window 清零；
5. 连续好窗口达到配置数量，且当前功率高于最小功率时，输出一次 SET 到相邻较低功率；
6. 输出 SET 后 good window 清零。

该组件当前不会主动“提高功率”。这是对现有固件行为的保持：坏链路只清除降功率迟滞窗口，不在这里生成升功率命令。

## API

- `iot_rf_power_policy_config_valid()`：校验功率表、范围和阈值配置；
- `iot_rf_power_policy_nearest_index()`：查找最接近的功率级索引；
- `iot_rf_power_policy_value_allowed()`：判断功率是否为范围内的有效表值；
- `iot_rf_power_policy_decide_auto()`：执行一次纯自动功率决策。

调用方需要保存 `decision.good_windows`，并在 `decision.action == IOT_RF_POWER_ACTION_SET` 时发送实际 RF 命令。

## 主机测试

```powershell
powershell -ExecutionPolicy Bypass -File firmware/common/iot_rf_power_policy/test/run_host_test.ps1
```

测试覆盖配置校验、最近功率级、范围校验、样本不足、越界、阈值相等、坏窗口清零、连续窗口降级、最低功率限制、计数器饱和和空参数。
