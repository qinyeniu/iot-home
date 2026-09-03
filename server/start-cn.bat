@echo off
chcp 65001 >nul
echo ========================================
echo   IoT-Home 本地开发环境
echo ========================================
echo.

echo [1/3] 检查 Docker 状态...
docker info >nul 2>&1
if errorlevel 1 (
    echo [X] Docker 未运行
    echo.
    echo 请先启动 Docker Desktop:
    echo 1. 点击开始菜单
    echo 2. 找到 Docker Desktop
    echo 3. 点击启动
    echo 4. 等待绿色图标
    echo.
    echo 启动后重新运行此脚本
    pause
    exit /b 1
)
echo [OK] Docker 已运行

echo.
echo [2/3] 启动服务...
docker compose up -d
if errorlevel 1 (
    echo [X] 启动失败
    echo 请查看错误信息
    pause
    exit /b 1
)

echo.
echo [3/3] 等待服务就绪...
timeout /t 15 /nobreak >nul

echo.
echo ========================================
echo   [OK] 所有服务已启动！
echo ========================================
echo.
echo 访问地址:
echo    * API 文档:  http://localhost:8000/docs
echo    * Grafana:   http://localhost:3000
echo.
echo Grafana 登录:
echo    * 用户名: admin
echo    * 密码:   grafana_2024
echo.
echo 下一步:
echo    1. 运行模拟设备: python tools\simulator.py
echo    2. 查看 API 文档: http://localhost:8000/docs
echo    3. 打开 Grafana:  http://localhost:3000
echo.
echo 按任意键退出...
pause >nul
