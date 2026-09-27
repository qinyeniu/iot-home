# iot_zb_node_model

Zigbee 应用节点模型组件。组件不依赖 Zigbee 协议栈、FreeRTOS、日志、MQTT 或硬件，可在电脑主机上直接编译测试。

当前阶段集中管理网关侧的节点表和应用数据：

- 短地址、IEEE 地址、在线状态和最后存活时间；
- 温度、湿度、照度原始值；
- On/Off 状态；
- Join / Leave / Report 对节点表的纯数据更新；
- 完整样本和部分样本的待发遥测选择；
- 遥测 JSON 组装。

实际 Zigbee 初始化、属性报告回调、APS 收发、加锁、日志、MQTT 发布和持久化仍由网关应用负责。

## 存储模型

调用方提供后端数组，组件不动态分配内存：

```c
iot_zb_node_t nodes[MAX_ZB_NODES];
const iot_zb_node_store_t store = {
    .nodes = nodes,
    .capacity = MAX_ZB_NODES,
};
```

`addr == 0` 表示空槽。Zigbee 协调器自身短地址为 0，子节点不会使用该地址。节点离开后保留短地址和最后状态，只把 `active` 置为 false 并清空 dirty；这保留现有网关行为。

## 数据字段

测量值沿用 Zigbee ZCL 原始编码：

- 温度：`int16_t`，单位 0.01°C，`0x8000` 为无效；
- 湿度：`uint16_t`，单位 0.01%，`0xFFFF` 为无效；
- 照度：`uint16_t`，编码为 `10000 * log10(lux + 1)`，`0xFFFF` 为无效。

## 主要 API

- `iot_zb_node_find()` / `iot_zb_node_find_ieee()`：查找节点槽位；
- `iot_zb_node_get_or_create()`：查找或创建节点；
- `iot_zb_node_apply_join()`：处理 Join / announce；
- `iot_zb_node_apply_leave()`：按 IEEE 处理 Leave；
- `iot_zb_node_apply_measurement()`：处理测量类属性报告；
- `iot_zb_node_apply_onoff()`：处理 On/Off 属性报告；
- `iot_zb_node_take_due()`：选择完整样本或达到部分等待时间的样本；
- `iot_zb_node_build_telemetry()`：组装业务遥测 JSON。

调用方应在自己的临界区或锁内调用会修改节点表的函数。模型本身没有静态可变状态，也不内置锁。

## 遥测选择

`iot_zb_node_take_due()` 保持旧网关规则：

1. 温度、湿度、照度三个 dirty 位全部到达时立即取出；
2. 只收到部分字段时，等待 `30000 ms`；
3. 取出后复制节点并清空 dirty，但不清空 valid；
4. On/Off 字段也会进入 JSON，但不参与“完整三要素”判断。

## 主机测试

```powershell
powershell -ExecutionPolicy Bypass -File firmware/common/iot_zb_node_model/test/run_host_test.ps1
```

测试覆盖空参数、表容量、短地址/IEEE 查找、Join/Leave、测量值和 On/Off 更新、无效值、完整样本立即发送、部分样本 30 秒边界、活动节点快照以及遥测 JSON 组装。