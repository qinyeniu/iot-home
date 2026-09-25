# RF Command Queue 目标生命周期测试

这是独立的 ESP32-C6 测试工程，直接编译生产网关的 `rf_command_queue.c`，不属于生产网关镜像；`main.c` 仍不调用该模块，且 `RF_COMMAND_QUEUE_ENABLED` 默认关闭。

## 当前覆盖

- 空闲状态 start → stop；
- 重复 start/stop 的错误返回；
- stop 后重新 start；
- submit 用户/周期命令后 stop；
- 队列对象和消费者任务的正常生命周期。

当前只保留离线构建通过证据；真实硬件运行、超时 latched cleanup 和并发 submit/stop 测试留待后续硬件窗口。

## 构建

```powershell
. C:\Espressif\v5.5.4\esp-idf\export.ps1
cd C:\Users\HJB\Documents\iot-home\firmware\gateway\tests\target_lifecycle
idf.py build
```

## 硬件运行

确认测试串口后，再执行：

```powershell
idf.py -p COMx flash monitor
```

成功时日志应出现：

```text
RF command queue lifecycle tests: PASS
```

失败时测试程序会记录 FAIL 并调用 `abort()`，避免自动观察漏判。该测试工程不得替代生产网关部署；也不应在长测期间打开 COM5/COM6。
