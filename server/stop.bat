@echo off
chcp 65001 >nul
echo ========================================
echo   IoT-Home Stop Script
echo ========================================
echo.

echo Stopping all services...
docker compose down

echo.
echo [OK] All services stopped
echo.
pause
