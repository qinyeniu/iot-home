# 明天待办事项

**日期**: 2026-08-27
**优先级**: 高

## 第一步：修复 ESP-IDF 环境问题

### 1.1 添加 Python 到 PATH
`powershell
C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell;C:\Users\HJB\.codex\tmp\arg0\codex-arg03OOfiB;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\bin\override;C:\Program Files (x86)\Common Files\Oracle\Java\javapath;F:\xuniji\bin\;C:\Program Files\Common Files\Oracle\Java\javapath;C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0\;C:\WINDOWS\System32\OpenSSH\;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell;C:\Users\Administrator\AppData\Local\Microsoft\WindowsApps;C:\Recovery\OEM\Backup\;C:\Program Files\dotnet\;C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0\;C:\WINDOWS\System32\OpenSSH\;F:\web\;C:\Program Files\NVIDIA Corporation\NVIDIA App\NvDLISR;C:\Program Files (x86)\NVIDIA Corporation\PhysX\Common;C:\Program Files\Git\cmd;C:\Program Files\Docker\Docker\resources\bin;C:\Users\HJB\Downloads;f:\trea\Trae\bin;C:\Users\HJB\AppData\Local\Programs\Eclipse Adoptium\jdk-21.0.10.7-hotspot\bin;C:\Program Files\MySQL\MySQL Shell 8.0\bin\;C:\Users\HJB\deveco studio\bin;F:\python\PyCharm 2025.2.1.1\bin;C:\Users\HJB\AppData\Roaming\npm;C:\Users\HJB\AppData\Local\GitHubDesktop\bin;C:\Users\HJB\AppData\Local\Python\bin;C:\Users\HJB\AppData\Local\Programs\Microsoft VS Code\bin;C:\Users\HJB\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Python\Python 3.14;C:\Users\HJB\AppData\Local\Microsoft\WindowsApps;F:\IntelliJ IDEA 2025.2.6.1\bin;C:\Users\HJB\AppData\Local\Programs\Warp\bin;C:\Users\HJB\AppData\Local\Python\pythoncore-3.14-64;C:\Users\HJB\AppData\Local\Python\pythoncore-3.14-64\Scripts;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\bin\fallback;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\git\cmd;C:\Program Files\WindowsApps\OpenAI.Codex_26.818.8289.0_x64__2p2nqsd0c76g0\app\resources += ";C:\Espressif\tools\python\v5.5.4\venv\Scripts"
`

### 1.2 验证 Python
`powershell
python --version
`

### 1.3 设置 ESP-IDF 环境
`powershell
cd C:\Espressif\v5.5.4\esp-idf
.\export.ps1
`

## 第二步：编译网关固件

### 2.1 进入固件目录
`powershell
cd C:\Users\HJB\Documents\iot-home\firmware\gateway
`

### 2.2 编译固件
`powershell
idf.py build
`

### 2.3 检查编译结果
- 确保没有错误
- 确保生成固件文件

## 第三步：烧录固件

### 3.1 连接 ESP32-C6 开发板
- 使用 USB-C 数据线连接电脑
- 检查串口（COM3 或其他）

### 3.2 烧录固件
`powershell
idf.py -p COM3 flash
`

### 3.3 查看日志
`powershell
idf.py -p COM3 monitor
`

## 第四步：验证功能

### 4.1 检查 Wi-Fi 连接
- 看到 "connected to ap SSID:qyn"
- 看到 "Got IP:" 信息

### 4.2 检查 MQTT 连接
- 看到 "MQTT_EVENT_CONNECTED"
- 看到 "Gateway initialized successfully!"

### 4.3 测试数据发送
- 检查 MQTT 服务器是否收到数据
- 检查 Grafana 是否显示数据

## 第五步：继续开发

### 5.1 实现 Zigbee 协调器
- 添加 Zigbee 初始化代码
- 实现设备发现和配对

### 5.2 实现 OLED 状态显示
- 添加 OLED 驱动
- 显示连接状态、IP 地址等

### 5.3 实现命令转发
- 接收 MQTT 命令
- 转发到 Zigbee 设备

## 重要提醒

### Wi-Fi 配置
- SSID: qyn
- Password: 20051030

### MQTT 配置
- Broker: mqtt://localhost:1883
- Topic Prefix: iot-home/gw-001

### 文件位置
- 网关固件: C:\Users\HJB\Documents\iot-home\firmware\gateway
- ESP-IDF: C:\Espressif\v5.5.4\esp-idf
- 进度文档: C:\Users\HJB\Documents\iot-home\docs\PROGRESS.md

## 故障排除

### 问题：Python 命令找不到
解决：
`powershell
C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell;C:\Users\HJB\.codex\tmp\arg0\codex-arg03OOfiB;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\bin\override;C:\Program Files (x86)\Common Files\Oracle\Java\javapath;F:\xuniji\bin\;C:\Program Files\Common Files\Oracle\Java\javapath;C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0\;C:\WINDOWS\System32\OpenSSH\;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell;C:\Users\Administrator\AppData\Local\Microsoft\WindowsApps;C:\Recovery\OEM\Backup\;C:\Program Files\dotnet\;C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0\;C:\WINDOWS\System32\OpenSSH\;F:\web\;C:\Program Files\NVIDIA Corporation\NVIDIA App\NvDLISR;C:\Program Files (x86)\NVIDIA Corporation\PhysX\Common;C:\Program Files\Git\cmd;C:\Program Files\Docker\Docker\resources\bin;C:\Users\HJB\Downloads;f:\trea\Trae\bin;C:\Users\HJB\AppData\Local\Programs\Eclipse Adoptium\jdk-21.0.10.7-hotspot\bin;C:\Program Files\MySQL\MySQL Shell 8.0\bin\;C:\Users\HJB\deveco studio\bin;F:\python\PyCharm 2025.2.1.1\bin;C:\Users\HJB\AppData\Roaming\npm;C:\Users\HJB\AppData\Local\GitHubDesktop\bin;C:\Users\HJB\AppData\Local\Python\bin;C:\Users\HJB\AppData\Local\Programs\Microsoft VS Code\bin;C:\Users\HJB\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Python\Python 3.14;C:\Users\HJB\AppData\Local\Microsoft\WindowsApps;F:\IntelliJ IDEA 2025.2.6.1\bin;C:\Users\HJB\AppData\Local\Programs\Warp\bin;C:\Users\HJB\AppData\Local\Python\pythoncore-3.14-64;C:\Users\HJB\AppData\Local\Python\pythoncore-3.14-64\Scripts;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\bin\fallback;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\git\cmd;C:\Program Files\WindowsApps\OpenAI.Codex_26.818.8289.0_x64__2p2nqsd0c76g0\app\resources += ";C:\Espressif\tools\python\v5.5.4\venv\Scripts"
`

### 问题：编译失败
解决：
1. 确保 ESP-IDF 环境正确设置
2. 检查错误信息
3. 清理并重新编译：idf.py fullclean

### 问题：烧录失败
解决：
1. 按住 BOOT 按钮再插入 USB
2. 检查串口连接
3. 尝试不同的串口

## 预期结果

烧录成功后，你应该看到：
`
IoT-Home Gateway Starting...
Initializing WiFi...
connected to ap SSID:qyn
Initializing MQTT...
MQTT_EVENT_CONNECTED
Gateway initialized successfully!
MQTT Broker: mqtt://localhost:1883
Topic Prefix: iot-home/gw-001
`

## 下一步

完成网关固件后，继续：
1. 开发传感器终端固件
2. 开发开关终端固件
3. 实现 Zigbee 组网
4. 集成测试
