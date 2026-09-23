# 网关 Zigbee 健康自愈与并发加锁加固

日期：2026-09-23  
适用固件：`firmware/gateway/main/main.c`、`firmware/node_zigbee/main/main.c`

## 1. 背景

整夜长测中，节点在前半段能够持续上报，但后续出现两类异常：

1. 节点日志出现一次：

   ```text
   assert failed: vPortExitCritical port.c:618 (port_uxCriticalNesting[0] > 0)
   ```

2. 网关后续出现持续的底层 ZBOSS Port/MAC 缓冲异常：

   ```text
   ZB_ESP_MAC: No buffer, handle it in the next iteration
   ```

分析整夜日志后确认：网关不是一启动就异常；在缓冲风暴开始前，私有功率事务已经多次返回 `ESP_ERR_NO_MEM`。这说明长期运行过程中存在非 Zigbee 主循环任务与 ZBOSS 调度并发访问的风险。

## 2. 根因判断

ESP Zigbee/ZBOSS 要求：

- Zigbee callback 内可以直接使用对应的 Zigbee API；
- FreeRTOS 普通任务、MQTT 事件回调、状态任务等非 callback 上下文调用 Zigbee API 时，必须通过全局 ZBOSS lock 串行化。

此前部分代码运行在普通任务中，却直接调用：

- `esp_zb_aps_data_request()`；
- `esp_zb_zcl_on_off_cmd_req()`。

这类调用可能与 ZBOSS 内部调度并发，长期运行后可能破坏端口层临界区状态或耗尽内部缓冲。

## 3. 已实施修改

### 3.1 非 callback 上下文统一加锁

网关：

- `rf_power_send_to_node()` 调用 `esp_zb_aps_data_request()` 前，先检查 `esp_zb_is_started()`，再获取/释放 ZBOSS lock；
- `gateway_handle_node_cmd()` 调用标准 ZCL On/Off API 前，同样检查栈状态并加锁。

节点：

- `rf_power_send_command()` 调用 `esp_zb_aps_data_request()` 前，检查栈状态并加锁。

所有日志输出均放在释放锁之后，减少锁持有时间。

### 3.2 精确识别底层 MAC 缓冲风暴

仅靠“没有收到节点帧”不能判断网关故障，因为节点可能被用户正常断电、带出门，或者进入未来的长周期睡眠。

因此网关安装了一个本地日志观察钩子：

- 不改变日志输出；
- 将格式化后的日志复制到临时缓冲区检查；
- 只识别同时包含 `ZB_ESP_MAC` 与 `No buffer, handle it` 的底层消息；
- 使用原子变量记录当前风暴的事件数、第一次事件时间、最近一次事件时间；
- 若两次事件间隔超过 5 秒，会重置当前风暴起点和计数，历史瞬时事件不会混入后续判断。

普通应用层 `ESP_ERR_NO_MEM` 不会直接触发重启；节点正常离线也不会触发。

## 4. 受控重启条件

网关只有在以下条件全部成立时才会受控重启：

1. 已记录过活跃子节点地址；
2. 已记录过该子节点的有效收包时间；
3. 连续 180 秒没有收到有效子节点帧；
4. 底层 MAC no-buffer 事件至少达到 100 次；
5. 第一批事件至今至少持续 60 秒；
6. 最近 5 秒内仍有 no-buffer 事件，说明风暴仍在持续，而不是历史瞬时异常。

重启前延迟 1 秒，尽量保证日志和离线状态刷出。

## 5. 明确不会触发的情况

以下场景不会因为本机制重启网关：

- 节点被用户正常拔掉；
- 节点带出门，网关没有任何活跃子节点；
- 节点未来进入超过 180 秒的正常长睡眠周期，但底层没有持续 no-buffer 风暴；
- 偶发几次 `No buffer` 后自动恢复；
- 私有功率命令偶发返回 `ESP_ERR_NO_MEM`，但底层没有持续缓冲风暴。

## 6. 已完成验证

- 网关修正版编译通过；
- 节点此前已完成 secure rejoin；
- 远程 QUERY 实测通过；
- 远程固定 0 dBm 实测通过；
- 随后固定 -10 dBm 并恢复 AUTO，最终状态为 `auto/-10 dBm`；
- 后端完整回归：`73 passed / 3 skipped`；
- 独立代码复审：P0=0、P1=0、P2 建议为未来长周期场景优化；
- 本次精确 MAC 风暴识别已进一步消除“节点正常离线导致误重启”的风险。

## 7. 固件备份

备份目录：

```text
backups/firmware/2026-09-23-lock-health-hardening/
```

当前修正版网关二进制：

```text
iot-home-gateway.bin
SHA256=8765D49E49BF81063A7E6CF3691E9184C20634792BF5C2620D9DFFB4A4C272B4
```

节点二进制：

```text
iot-home-zigbee-node.bin
SHA256=58D2C75EC14DD0EDFE3D6EC340B6C59A179185F40058CCD33417E1FEADBC5DBB
```

## 8. 节点重新接入后的待验证项

1. 网关启动后节点 secure rejoin 是否成功；
2. QUERY / SET / AUTO 的完整事务序列是否仍然通过；
3. 网关在节点正常拔掉后是否保持运行、不发生误重启；
4. 长时间运行后是否还会出现 `vPortExitCritical` 或底层 no-buffer 风暴。

## 9. 安全边界

本次只编译和备份固件；未刷 bootloader、分区表，未擦 Zigbee NVRAM，未部署服务器，未 push。任何密码文件均未读取、打印或写入文档。
