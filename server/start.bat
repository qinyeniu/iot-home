@echo off
chcp 65001 >nul
echo ========================================
echo   IoT-Home Local Dev Environment
echo ========================================
echo.

echo [1/4] Checking Docker status...
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
echo [2/4] Creating .env file (if not exists)...
if not exist ".env" (
    copy .env.example .env >nul
    echo [OK] .env file created
) else (
    echo [OK] .env file exists
)

echo.
echo [3/4] Starting services...
echo Starting Mosquitto, MySQL, FastAPI, Grafana...
docker compose up -d
if errorlevel 1 (
    echo [X] Failed to start
    pause
    exit /b 1
)

echo.
echo [4/4] Waiting for services to be ready...
timeout /t 10 /nobreak >nul

echo.
echo ========================================
echo   [OK] All services started!
echo ========================================
echo.
echo Access URLs:
echo    * FastAPI Backend: http://localhost:8000
echo    * API Docs:        http://localhost:8000/docs
echo    * Grafana:         http://localhost:3000
echo    * MQTT Port:       localhost:1883
echo    * MySQL Port:      localhost:3307
echo.
echo Default Login:
echo    * Grafana: admin / grafana_2024
echo    * MySQL:   iot_home / iot_mysql_2024
echo    * MQTT:    iot_user / iot_mqtt_2024
echo.
echo Next Steps:
echo    1. Run simulator: python tools\simulator.py
echo    2. View API docs: http://localhost:8000/docs
echo    3. Open Grafana:  http://localhost:3000
echo.
pause
