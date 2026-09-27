# iot_rf_transaction

RF 私有命令事务的纯逻辑组件。组件不依赖 ESP-IDF、FreeRTOS、Zigbee 或硬件，可在电脑主机上直接编译测试。

当前阶段只拆出网关既有的 QUERY / 等待 REPORT / 超时重查 / 周期 reconcile 判断，以及 REPORT 字段匹配；实际 Zigbee 发送、加密、APS ACK、事务字段写入和 OLED/MQTT 状态更新仍由网关应用负责。

## 配置

`iot_rf_transaction_config_t` 使用调用方提供的毫秒时间基：

- `link_fresh_ms`：超过该时间未收到节点链路数据时判定链路过期；
- `report_timeout_ms`：等待 REPORT 达到该时间后允许重新 QUERY；
- `reconcile_ms`：非等待状态下，距上次确认达到该时间后执行周期 QUERY。

当前网关配置：

```text
link fresh     = 120000 ms
report timeout = 90000 ms
reconcile      = 300000 ms
```

## 周期决策

`iot_rf_transaction_evaluate()` 按以下顺序执行，保持旧网关逻辑：

1. `now - last_rx > link_fresh_ms`：返回 `STALE`；
2. 当前功率未知：返回 `QUERY / CURRENT_UNKNOWN`；
3. 正在等待且等待时间达到 `report_timeout_ms`：返回 `QUERY / WAIT_TIMEOUT`；
4. 非等待状态、已有确认且距上次确认达到 `reconcile_ms`：返回 `QUERY / RECONCILE`；
5. 正在等待 REPORT：返回 `WAIT`；
6. 其他情况：返回 `CONTINUE`。

时间差使用 `uint32_t` 减法，因此兼容正常的 32 位 tick 回绕。`last_confirmed_ms == 0` 表示尚未确认，不会触发 reconcile。

调用方收到 `STALE` 后负责清理等待状态并触发后续策略；收到 `QUERY` 后负责发送实际私有 QUERY；收到 `WAIT` 或 `CONTINUE` 时不发送本事务生成的 QUERY。

## REPORT 匹配

`iot_rf_transaction_report_matches()` 用于比较期望事务和节点 REPORT：

- sequence 必须始终匹配；
- expected power 为 `-128` 时忽略功率字段；
- expected mode 为 `0xFF` 时忽略模式字段；
- 其他功率和模式必须精确相等。

调用方仍需先确认数据来自有效 V2 RF packet，并且网关当前确实处于相应等待状态。

## 主机测试

```powershell
powershell -ExecutionPolicy Bypass -File firmware/common/iot_rf_transaction/test/run_host_test.ps1
```

测试覆盖非法参数、链路过期边界、当前功率未知、REPORT 等待超时边界、reconcile 边界、零确认、等待状态抑制 reconcile、tick 回绕，以及 REPORT 的 sequence/power/mode 匹配和可选字段。