# RF 命令队列 CQ-2 硬件短测手册

更新时间：2026-09-25
适用固件：网关实验构建 `RF_COMMAND_QUEUE_ENABLED=ON`
状态：等待硬件维护窗口；当前不自动烧录、不部署

## 1. 测试目标

验证用户侧四类操作经单一 RF 队列消费者执行：

```text
QUERY -> SET 0 -> SET -10 -> AUTO
```

同时验证：

1. 每条用户命令都有 submit 和 dequeue 记录；
2. 每个用户策略最多产生一个 RF 私有 Zigbee 帧；
3. `ESP_OK` 后收到节点加密 REPORT；
4. `rf-dispatch` 栈高水位安全；
5. 节点正常拔掉时网关不重启；
6. 节点接回后能安全恢复。

## 2. 硬性边界

- 只烧录网关应用区；
- 不执行整片擦除；
- 不刷 bootloader；
- 不刷分区表；
- 不擦 Zigbee NVRAM；
- 不切换网卡、不改网络配置；
- 不部署云服务器；
- 打开串口监视器可能触发热复位，这是已知现象。

## 3. 维护窗口步骤

### 3.1 确认串口

先确认：

- `COM6`：网关；
- `COM5`：节点。

如果系统枚举端口变化，以设备管理器实际端口为准，不要猜测。

### 3.2 构建实验网关固件

```powershell
. C:\Espressif\v5.5.4\esp-idf\export.ps1
cd C:\Users\HJB\Documents\iot-home\firmware\gateway
idf.py -DRF_COMMAND_QUEUE_ENABLED=ON build
```

当前离线构建记录：

- 实验 ON 应用大小：`0x1604e0`；
- 应用分区剩余：4%。

### 3.3 只烧录网关应用区

```powershell
idf.py -p COM6 app-flash
```

不要使用 `erase-flash`，也不要烧录 `bootloader.bin` 或 `partition-table.bin`。

### 3.4 启动双端日志记录

建议准备独立日志目录，例如：

```powershell
cd C:\Users\HJB\Documents\iot-home
python tools/capture_serial_no_reset.py COM6 backups/cq-short-YYYYMMDD/gateway.log 900 --overwrite
python tools/capture_serial_no_reset.py COM5 backups/cq-short-YYYYMMDD/node.log 900 --overwrite
```

打开串口仍可能导致板子复位；因此烧录后先启动网关日志，再启动节点日志，并把复位时间记录到交接文档。

### 3.5 发起业务操作

按顺序通过现有后端或前端发起：

1. QUERY；
2. SET `0` dBm；
3. SET `-10` dBm；
4. AUTO。

每一步等待网关状态进入 confirmed，或明确失败后再继续，避免连续点击造成测试结论混淆。

### 3.6 节点拔插

四项操作完成后：

1. 正常拔掉节点；
2. 继续观察网关至少 60 秒；
3. 确认网关没有重启；
4. 接回节点；
5. 等待 secure rejoin 和后续遥测恢复。

## 4. 自动判读

```powershell
python tools/analyze_rf_cq_short_test.py `
  backups/cq-short-YYYYMMDD/gateway.log `
  --node-log backups/cq-short-YYYYMMDD/node.log
```

结论含义：

- `PASS`：配对、顺序、栈和 REPORT 均通过；
- `DEGRADED`：没有致命失败，但存在未确认、重复拒绝、栈偏低等问题；
- `FAIL`：出现重启、uptime 回跳、致命错误、非用户命令被消费者执行等；
- `LEGACY/INCOMPLETE`：日志中没有 CQ 队列标记，可能运行的是默认 OFF 固件。

## 5. 通过标准

短测通过必须同时满足：

1. 日志中出现 submit/dequeue；
2. 操作顺序为 QUERY -> SET 0 -> SET -10 -> AUTO；
3. 每个用户策略只观察到一个对应 RF 私有下行帧；
4. 每个 `ESP_OK` 都有匹配 seq 的加密 REPORT；
5. `rf-dispatch` 栈剩余不得低于 512 字节；低于 256 字节直接 FAIL；
6. 节点拔掉期间网关不重启；
7. 节点接回后业务恢复。

## 6. 回退方法

若实验 ON 固件异常，使用默认 OFF 构建并只烧录网关应用区：

```powershell
cd C:\Users\HJB\Documents\iot-home\firmware\gateway
idf.py -DRF_COMMAND_QUEUE_ENABLED=OFF build
idf.py -p COM6 app-flash
```

该回退不需要改分区表、不需要擦 Flash，也不依赖云服务器变更。
