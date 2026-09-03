@echo off
chcp 65001 >nul
echo ========================================
echo   IoT-Home Local Dev Environment
echo ========================================
echo.

echo [1/3] Checking Docker status...
docker info >nul 2>&1
if errorlevel 1 (
    echo [X] Docker is not running
    echo.
    echo Please start Docker Desktop first:
    echo 1. Click Start Menu
    echo 2. Find Docker Desktop
    echo 3. Click to start
    echo 4. Wait for green icon
    echo.
    echo After starting, run this script again
    pause
    exit /b 1
)
echo [OK] Docker is running

echo.
echo [2/3] Starting services...
docker compose up -d
if errorlevel 1 (
    echo [X] Failed to start
    echo Please check error messages
    pause
    exit /b 1
)

echo.
echo [3/3] Waiting for services to be ready...
timeout /t 15 /nobreak >nul

echo.
echo ========================================
echo   [OK] All services started!
echo ========================================
echo.
echo Access URLs:
echo    * API Docs:  http://localhost:8000/docs
echo    * Grafana:   http://localhost:3000
echo.
echo Grafana Login:
echo    * Username: admin
echo    * Password: grafana_2024
echo.
echo Next Steps:
echo    1. Run simulator: python tools\simulator.py
echo    2. View API docs: http://localhost:8000/docs
echo    3. Open Grafana:  http://localhost:3000
echo.
echo Press any key to exit...
pause >nul
