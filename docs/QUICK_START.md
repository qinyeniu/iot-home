# 🚀 IoT-Home 启动指南（一步一步）

## 当前状态
✅ Docker Desktop 已启动
⏳ Docker 服务正在初始化中

## 下一步：等待 Docker 完全启动

### 如何判断 Docker 已就绪？

**方法 1：查看 Docker Desktop 界面**
1. 打开 Docker Desktop 应用
2. 查看左下角状态图标：
   - 🟢 **绿色**：Docker 已就绪，可以继续
   - 🟡 **黄色**：Docker 正在启动，请等待
   - 🔴 **红色**：Docker 未运行，需要启动

**方法 2：使用命令验证**
1. 打开 PowerShell 窗口
2. 输入：docker info
3. 如果看到 Server Version: 29.6.1，说明已就绪

## Docker 就绪后：启动 IoT-Home 服务

### 步骤 1：打开 PowerShell
- 按 **Windows 键 + R**
- 输入 **powershell**
- 回车

### 步骤 2：进入项目目录
`powershell
cd C:\Users\HJB\Documents\iot-home\server
`

### 步骤 3：运行启动脚本
`powershell
.\start.bat
`

### 步骤 4：等待服务启动
你会看到：
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

## 服务启动后：访问和测试

### 1. 访问 API 文档
- 打开浏览器
- 访问：http://localhost:8000/docs
- 你会看到 Swagger API 文档界面

### 2. 访问 Grafana 看板
- 打开浏览器
- 访问：http://localhost:3000
- 登录：
  - 用户名：admin
  - 密码：grafana_2024

### 3. 运行模拟设备
- 打开新的 PowerShell 窗口
- 运行：
`powershell
cd C:\Users\HJB\Documents\iot-home
pip install paho-mqtt
python tools\simulator.py
`

## 常用命令

### 查看服务状态
`powershell
docker compose ps
`

### 查看服务日志
`powershell
docker compose logs -f
`

### 停止服务
`powershell
.\stop.bat
`

### 重启服务
`powershell
docker compose restart
`

## 故障排除

### 问题：Docker 启动很慢
**解决**：
- 耐心等待，首次启动需要 2-5 分钟
- 查看 Docker Desktop 界面进度
- 不要关闭 Docker Desktop

### 问题：端口被占用
**解决**：
`powershell
# 查找占用端口的进程
netstat -ano | findstr :3306

# 停止进程
taskkill /PID <进程ID> /F
`

### 问题：服务启动失败
**解决**：
`powershell
# 查看详细日志
docker compose logs

# 完全重置
docker compose down -v
docker compose up -d
`

## 下一步

1. ✅ Docker Desktop 已启动
2. ⏳ 等待 Docker 服务完全就绪
3. 📋 启动 IoT-Home 服务
4. 🎯 访问和测试服务

## 需要帮助？

如果遇到问题：
1. 查看日志：docker compose logs
2. 重启服务：docker compose restart
3. 完全重置：docker compose down -v && docker compose up -d
4. 查看文档：docs/DEVELOPMENT_GUIDE.md
