# Docker Desktop 使用教程

## 什么是 Docker Desktop？

Docker Desktop 是一个让你在 Windows 上运行 Docker 容器的工具。
简单来说，它能把我们的 IoT 服务（MQTT、MySQL、FastAPI、Grafana）
打包成独立的"容器"，一键启动，不需要手动安装每个软件。

## 第一步：找到并启动 Docker Desktop

### 方法 1：从开始菜单启动

1. 点击 Windows 左下角的 **开始菜单**（Windows 图标）
2. 在程序列表中找到 **"Docker Desktop"**
3. 点击启动

### 方法 2：从任务栏启动

1. 查看电脑右下角的 **系统托盘**（时钟旁边）
2. 如果看到 Docker 图标（鲸鱼形状），说明已经安装
3. 右键点击 Docker 图标，选择 **"Dashboard"** 打开控制面板

### 方法 3：从搜索启动

1. 按 **Windows 键** 或点击搜索框
2. 输入 **"Docker"**
3. 点击 **"Docker Desktop"** 应用

## 第二步：等待 Docker 启动

启动 Docker Desktop 后，需要等待它完全加载：

### 启动过程中的状态

1. **启动中**：你会看到 Docker Desktop 界面显示 "Starting..."
2. **加载中**：底部状态栏会显示进度条
3. **就绪**：当看到 **绿色对勾 ✅** 或 **"Docker Desktop is running"** 时，表示已就绪

### 启动时间

- 首次启动：可能需要 **2-5 分钟**
- 后续启动：通常 **30秒-1分钟**

### 常见启动问题

**问题 1：提示需要启用虚拟化**
- 解决：需要进入 BIOS 启用虚拟化（VT-x/AMD-V）
- 或者：启用 Windows 功能 "Hyper-V" 和 "Windows Subsystem for Linux"

**问题 2：提示 WSL 2 安装**
- 解决：按照提示安装 WSL 2（Windows Subsystem for Linux 2）
- 或者：使用 Hyper-V 后端（在设置中切换）

**问题 3：启动很慢**
- 解决：耐心等待，首次启动确实需要较长时间
- 可以查看任务管理器，确认 Docker 服务在运行

## 第三步：验证 Docker 是否正常运行

### 方法 1：使用 Docker Desktop 界面

1. 打开 Docker Desktop
2. 查看左下角状态：
   - ✅ **绿色**：Docker 运行正常
   - ⚠️ **黄色**：Docker 正在启动或有问题
   - ❌ **红色**：Docker 未运行

### 方法 2：使用命令行验证

1. 按 **Windows 键 + R**，输入 **"powershell"**，回车
2. 输入以下命令：

`powershell
docker --version
`

应该看到类似输出：
`
Docker version 29.6.1, build 8900f1d
`

3. 再输入：

`powershell
docker info
`

应该看到很多信息，最重要的是：
`
Server Version: 29.6.1
`

如果看到错误，说明 Docker 还没完全启动，请继续等待。

## 第四步：启动 IoT-Home 服务

### 步骤 1：打开 PowerShell

1. 按 **Windows 键 + R**
2. 输入 **"powershell"**
3. 回车

### 步骤 2：进入项目目录

`powershell
cd C:\Users\HJB\Documents\iot-home\server
`

### 步骤 3：运行启动脚本

`powershell
.\start.bat
`

### 步骤 4：等待服务启动

你会看到类似输出：
`
========================================
  IoT-Home 本地开发环境启动脚本
========================================

[1/4] 检查 Docker 状态...
✅ Docker 已运行

[2/4] 创建 .env 文件（如果不存在）...
✅ .env 文件已存在

[3/4] 启动服务...
正在启动 Mosquitto、MySQL、FastAPI、Grafana...
[+] Running 4/4
 ✔ Container iot-mosquitto  Started
 ✔ Container iot-mysql      Started
 ✔ Container iot-backend    Started
 ✔ Container iot-grafana    Started

[4/4] 等待服务就绪...

========================================
  ✅ 所有服务已启动！
========================================
`

### 步骤 5：验证服务运行

在另一个 PowerShell 窗口输入：

`powershell
docker compose ps
`

应该看到 4 个容器都在运行：
`
NAME                STATUS          PORTS
iot-mosquitto       Up 2 minutes    0.0.0.0:1883->1883/tcp
iot-mysql           Up 2 minutes    0.0.0.0:3306->3306/tcp
iot-backend         Up 2 minutes    0.0.0.0:8000->8000/tcp
iot-grafana         Up 2 minutes    0.0.0.0:3000->3000/tcp
`

## 第五步：访问服务

### 1. 访问 API 文档

1. 打开浏览器
2. 访问：http://localhost:8000/docs
3. 你会看到 Swagger API 文档界面
4. 可以在这里测试所有 API 接口

### 2. 访问 Grafana 看板

1. 打开浏览器
2. 访问：http://localhost:3000
3. 登录：
   - 用户名：admin
   - 密码：grafana_2024
4. 点击左侧菜单 **"Dashboards"**
5. 找到 **"IoT-Home 环境监测"** 看板

### 3. 运行模拟设备

1. 打开新的 PowerShell 窗口
2. 进入项目目录：

`powershell
cd C:\Users\HJB\Documents\iot-home
`

3. 安装依赖（如果还没安装）：

`powershell
pip install paho-mqtt
`

4. 运行模拟设备：

`powershell
python tools\simulator.py
`

5. 你会看到模拟设备开始发送数据

## 第六步：查看数据

### 在 Grafana 中查看

1. 打开 Grafana 看板
2. 选择设备（如果有的话）
3. 你会看到实时更新的温度、湿度、光照曲线

### 在 API 中查看

1. 访问 http://localhost:8000/docs
2. 找到 **"GET /api/metrics/latest"** 接口
3. 点击 **"Try it out"**
4. 点击 **"Execute"**
5. 查看返回的数据

## 常用命令

### 查看服务状态
`powershell
docker compose ps
`

### 查看服务日志
`powershell
# 查看所有服务日志
docker compose logs

# 查看特定服务日志
docker compose logs backend
docker compose logs mysql
docker compose logs mosquitto
docker compose logs grafana

# 实时查看日志
docker compose logs -f
`

### 停止服务
`powershell
# 停止所有服务
docker compose down

# 或使用停止脚本
.\stop.bat
`

### 重启服务
`powershell
# 重启所有服务
docker compose restart

# 重启特定服务
docker compose restart backend
`

### 进入容器
`powershell
# 进入 MySQL 容器
docker compose exec mysql mysql -u iot_home -piot_mysql_2024 iot_home

# 进入 Mosquitto 容器
docker compose exec mosquitto sh
`

## 故障排除

### 问题 1：Docker 启动失败

**症状**：Docker Desktop 无法启动，或启动后显示错误

**解决方案**：
1. 确保电脑支持虚拟化（Intel VT-x 或 AMD-V）
2. 进入 BIOS 启用虚拟化
3. 启用 Windows 功能：
   - 控制面板 -> 程序 -> 启用或关闭 Windows 功能
   - 勾选 "Hyper-V" 和 "Windows Subsystem for Linux"
   - 重启电脑

### 问题 2：端口被占用

**症状**：启动时报错 "port is already allocated"

**解决方案**：
1. 查找占用端口的进程：
`powershell
netstat -ano | findstr :3306
`

2. 停止占用端口的进程：
`powershell
taskkill /PID <进程ID> /F
`

3. 或修改 docker-compose.yml 使用其他端口

### 问题 3：服务启动慢

**症状**：服务启动需要很长时间

**解决方案**：
1. 耐心等待，首次启动确实需要较长时间
2. 查看日志了解进度：
`powershell
docker compose logs -f
`

3. 如果 MySQL 启动慢，可能是初始化脚本执行中

### 问题 4：无法访问服务

**症状**：浏览器无法访问 localhost:8000 或 localhost:3000

**解决方案**：
1. 检查服务是否运行：
`powershell
docker compose ps
`

2. 检查端口是否监听：
`powershell
netstat -ano | findstr :8000
netstat -ano | findstr :3000
`

3. 检查防火墙设置

## 下一步

成功启动服务后，你可以：

1. **查看 API 文档**：http://localhost:8000/docs
2. **运行模拟设备**：python tools\simulator.py
3. **查看 Grafana 看板**：http://localhost:3000
4. **开始固件开发**：参考 docs/DEVELOPMENT_GUIDE.md

## 获取帮助

如果遇到问题：

1. **查看日志**：docker compose logs
2. **重启服务**：docker compose restart
3. **完全重置**：docker compose down -v && docker compose up -d
4. **查看文档**：docs/DEVELOPMENT_GUIDE.md
