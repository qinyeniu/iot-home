# IoT-Home 项目进度文档

**最后更新**: 2026-08-26 21:45
**当前阶段**: 固件开发 - 网关固件

## 一、今日完成

### 1. 服务器环境 ✅
- Docker Desktop 配置完成
- Mosquitto MQTT 服务运行中
- MySQL 数据库运行中
- FastAPI 后端运行中
- Grafana 可视化运行中
- 模拟设备测试成功

### 2. 网关固件项目 ✅
- ESP-IDF v5.5.4 已安装
- 项目结构已创建
- Wi-Fi STA 连接代码已编写
- MQTT 客户端代码已编写
- Wi-Fi 配置已修改（SSID: qyn, Password: 20051030）

## 二、当前状态

### ESP-IDF 环境
- 安装位置: C:\Espressif\v5.5.4\esp-idf
- Python 环境: 已安装，但需要添加到 PATH
- export.ps1: 需要修复 Python PATH 问题

### 待解决问题
- Python 命令找不到（需要添加到 PATH）
- ESP-IDF 终端无法正常打开

## 三、明天继续

### 步骤 1：修复 Python PATH
`powershell
# 添加 Python 到 PATH
C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell;C:\Users\HJB\.codex\tmp\arg0\codex-arg03OOfiB;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\bin\override;C:\Program Files (x86)\Common Files\Oracle\Java\javapath;F:\xuniji\bin\;C:\Program Files\Common Files\Oracle\Java\javapath;C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0\;C:\WINDOWS\System32\OpenSSH\;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell;C:\Users\Administrator\AppData\Local\Microsoft\WindowsApps;C:\Recovery\OEM\Backup\;C:\Program Files\dotnet\;C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0\;C:\WINDOWS\System32\OpenSSH\;F:\web\;C:\Program Files\NVIDIA Corporation\NVIDIA App\NvDLISR;C:\Program Files (x86)\NVIDIA Corporation\PhysX\Common;C:\Program Files\Git\cmd;C:\Program Files\Docker\Docker\resources\bin;C:\Users\HJB\Downloads;f:\trea\Trae\bin;C:\Users\HJB\AppData\Local\Programs\Eclipse Adoptium\jdk-21.0.10.7-hotspot\bin;C:\Program Files\MySQL\MySQL Shell 8.0\bin\;C:\Users\HJB\deveco studio\bin;F:\python\PyCharm 2025.2.1.1\bin;C:\Users\HJB\AppData\Roaming\npm;C:\Users\HJB\AppData\Local\GitHubDesktop\bin;C:\Users\HJB\AppData\Local\Python\bin;C:\Users\HJB\AppData\Local\Programs\Microsoft VS Code\bin;C:\Users\HJB\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Python\Python 3.14;C:\Users\HJB\AppData\Local\Microsoft\WindowsApps;F:\IntelliJ IDEA 2025.2.6.1\bin;C:\Users\HJB\AppData\Local\Programs\Warp\bin;C:\Users\HJB\AppData\Local\Python\pythoncore-3.14-64;C:\Users\HJB\AppData\Local\Python\pythoncore-3.14-64\Scripts;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\bin\fallback;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\git\cmd;C:\Program Files\WindowsApps\OpenAI.Codex_26.818.8289.0_x64__2p2nqsd0c76g0\app\resources += ";C:\Espressif\tools\python\v5.5.4\venv\Scripts"

# 验证 Python
python --version
`

### 步骤 2：设置 ESP-IDF 环境
`powershell
cd C:\Espressif\v5.5.4\esp-idf
.\export.ps1
`

### 步骤 3：编译固件
`powershell
cd C:\Users\HJB\Documents\iot-home\firmware\gateway
idf.py build
`

### 步骤 4：烧录固件
`powershell
idf.py -p COM3 flash
`

### 步骤 5：查看日志
`powershell
idf.py -p COM3 monitor
`

## 四、重要文件位置

### 服务器环境
- 项目目录: C:\Users\HJB\Documents\iot-home
- 启动脚本: server\start-simple.bat
- 配置文件: server\.env

### 固件开发
- 网关固件: firmware\gateway
- 主程序: firmware\gateway\main\main.c
- 配置文件: firmware\gateway\sdkconfig.defaults
- ESP-IDF: C:\Espressif\v5.5.4\esp-idf

## 五、Wi-Fi 配置

`properties
CONFIG_ESP_WIFI_SSID="qyn"
CONFIG_ESP_WIFI_PASSWORD="20051030"
`

## 六、MQTT 配置

`c
#define MQTT_BROKER_URI "mqtt://localhost:1883"
#define MQTT_TOPIC_PREFIX "iot-home/gw-001"
`

## 七、预期结果

烧录成功后，你会看到：
`
IoT-Home Gateway Starting...
Initializing WiFi...
connected to ap SSID:qyn
Initializing MQTT...
MQTT_EVENT_CONNECTED
Gateway initialized successfully!
`

## 八、故障排除

### 问题：Python 命令找不到
解决：添加到 PATH
`powershell
C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell;C:\Users\HJB\.codex\tmp\arg0\codex-arg03OOfiB;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\bin\override;C:\Program Files (x86)\Common Files\Oracle\Java\javapath;F:\xuniji\bin\;C:\Program Files\Common Files\Oracle\Java\javapath;C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0\;C:\WINDOWS\System32\OpenSSH\;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell;C:\Users\Administrator\AppData\Local\Microsoft\WindowsApps;C:\Recovery\OEM\Backup\;C:\Program Files\dotnet\;C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0\;C:\WINDOWS\System32\OpenSSH\;F:\web\;C:\Program Files\NVIDIA Corporation\NVIDIA App\NvDLISR;C:\Program Files (x86)\NVIDIA Corporation\PhysX\Common;C:\Program Files\Git\cmd;C:\Program Files\Docker\Docker\resources\bin;C:\Users\HJB\Downloads;f:\trea\Trae\bin;C:\Users\HJB\AppData\Local\Programs\Eclipse Adoptium\jdk-21.0.10.7-hotspot\bin;C:\Program Files\MySQL\MySQL Shell 8.0\bin\;C:\Users\HJB\deveco studio\bin;F:\python\PyCharm 2025.2.1.1\bin;C:\Users\HJB\AppData\Roaming\npm;C:\Users\HJB\AppData\Local\GitHubDesktop\bin;C:\Users\HJB\AppData\Local\Python\bin;C:\Users\HJB\AppData\Local\Programs\Microsoft VS Code\bin;C:\Users\HJB\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Python\Python 3.14;C:\Users\HJB\AppData\Local\Microsoft\WindowsApps;F:\IntelliJ IDEA 2025.2.6.1\bin;C:\Users\HJB\AppData\Local\Programs\Warp\bin;C:\Users\HJB\AppData\Local\Python\pythoncore-3.14-64;C:\Users\HJB\AppData\Local\Python\pythoncore-3.14-64\Scripts;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\bin\fallback;C:\Users\HJB\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\git\cmd;C:\Program Files\WindowsApps\OpenAI.Codex_26.818.8289.0_x64__2p2nqsd0c76g0\app\resources += ";C:\Espressif\tools\python\v5.5.4\venv\Scripts"
`

### 问题：export.ps1 失败
解决：使用完整路径
`powershell
& "C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe" "tools/activate.py" --export
`

### 问题：编译失败
解决：确保 ESP-IDF 环境正确设置

### 问题：烧录失败
解决：按住 BOOT 按钮再插入 USB

## 九、下一步计划

1. ✅ 服务器环境搭建
2. ✅ 网关固件项目创建
3. ⏳ 修复 ESP-IDF 环境问题
4. ⏳ 编译和烧录网关固件
5. ⏳ 测试 Wi-Fi 和 MQTT 连接
6. ⏳ 实现 Zigbee 协调器
7. ⏳ 实现 OLED 状态显示
8. ⏳ 开发传感器终端固件
9. ⏳ 开发开关终端固件
10. ⏳ 实现 Zigbee 组网

## 十、参考资源

- ESP-IDF 安装文档: docs\ESP-IDF工具链安装.md
- 网关固件说明: firmware\gateway\README.md
- 服务器环境: server\README.md
