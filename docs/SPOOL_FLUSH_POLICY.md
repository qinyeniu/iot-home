# Flash Spool 写入节流策略

更新时间：2026-09-25  
状态：阶段 B1 已离线实现并通过主机测试；尚未接入网关、未写 Flash、未改分区表  
适用对象：后续 ESP32-C6 网关专用 spool 分区

## 1. 目标

把“什么时候允许把 RAM 中的断网数据批量写入 Flash”独立成可测试策略，避免未来接入 spool 后出现一条一写、频繁擦写或短时间写入过量。

当前模块只做判断和计数，不执行 NVS/Flash I/O。

## 2. 默认阈值

| 参数 | 默认值 | 作用 |
|---|---:|---|
| 最小写入间隔 | 60 秒 | 避免连续擦写 |
| 批量条数 | 16 条 | RAM 攒够一批才落盘 |
| 最长 RAM 保留时间 | 120 秒 | 防止数据长期只在 RAM |
| 每小时写入预算 | 10 批 | 控制短时间磨损 |
| 每日写入预算 | 20 批 | 控制长期磨损 |

一次“允许写入”对应一次完整的批量 Flash 写事务，不是每条记录写一次。

## 3. 判定规则

正常模式必须满足：

1. 有未写入数据；
2. 达到 16 条，或最旧数据已满 120 秒；
3. 距离上次成功写入至少 60 秒；
4. 小时和每日预算未用完。

强制模式只用于未来受控的维护/掉电前流程：

- 可跳过批量数量、数据年龄和最小间隔；
- 不能跳过小时/每日预算；
- 当前硬件没有电源失效预警，因此暂不提供真实调用入口。

## 4. 调用契约

未来接入真实 Flash spool 时必须满足：

1. 状态结构零初始化，除本组件和测试外不直接修改计数；
2. `now_ms`、`oldest_dirty_ms` 和窗口时间必须来自同一个单调 uptime 毫秒时钟，不能使用墙钟/NTP 时间；
3. 只能有一个 writer，并由它把 `should_flush()` → Flash 写事务 → `on_flush()` 放进同一个互斥/临界区；
4. 只有批量 Flash 写事务真正成功后，才调用 `on_flush()`。

当前 check/commit 分离的设计本身不预留预算，因此如果未来存在多个并发 writer，需要改为 reserve/commit API；否则不能接入。

计数器使用饱和递增，极端误调用达到 `UINT16_MAX` 后不会回绕成 0。

## 5. 时间回绕

时间戳使用 32 位毫秒计数，约 49.7 天回绕。策略使用无符号差值，因此“旧时间 → 新时间回绕”仍可计算间隔。测试已覆盖跨越回绕点的 120 秒年龄阈值、59 秒最小间隔拦截和 60 秒放行。

## 6. 文件

```text
firmware/common/iot_spool_policy/include/iot_spool_policy.h
firmware/common/iot_spool_policy/iot_spool_policy.c
firmware/common/iot_spool_policy/tests/test_iot_spool_policy.c
```

## 7. 测试

```powershell
gcc -std=c11 -Wall -Wextra -Werror `
  -Ifirmware/common/iot_spool_policy/include `
  firmware/common/iot_spool_policy/tests/test_iot_spool_policy.c `
  firmware/common/iot_spool_policy/iot_spool_policy.c `
  -o backups/tmp/iot-spool-policy-host/test_iot_spool_policy.exe

.\backups\tmp\iot-spool-policy-host\test_iot_spool_policy.exe
```

普通和 `-DNDEBUG` 模式均通过。测试覆盖：

- 空数据、参数错误；
- 批量/年龄触发；
- 最小间隔；
- 强制模式不能突破小时/每日预算；
- 小时/每日预算；
- 查询只旋转本地副本，`on_flush()` 后真实窗口和计数才更新；
- 跨越 32 位毫秒计数回绕的年龄/间隔阈值；
- 计数饱和。

## 8. 后续接入条件

1. 用户确认新增专用 spool 分区，并允许更新/烧录分区表；
2. 实现固定大小记录、CRC、双索引和环形擦除；
3. 网络恢复后的补发限速必须先完成；
4. 强制断电恢复测试通过；
5. 再由单一 spool writer 调用本策略；
6. 硬件实测 Flash 写入次数和磨损后，才决定默认启用。

因此当前阶段不会改变现网 RAM outbox，也不会把遥测写入现有 NVS。
