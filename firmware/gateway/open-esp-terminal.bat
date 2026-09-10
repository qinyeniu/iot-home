@echo off
echo ========================================
echo   ESP-IDF v5.5.4 Terminal
echo ========================================
echo.

echo Setting up ESP-IDF environment...
cd C:\Espressif\v5.5.4\esp-idf
call export.ps1

echo.
echo ========================================
echo   ESP-IDF environment ready!
echo ========================================
echo.
echo You can now use idf.py commands:
echo   - idf.py build      (compile firmware)
echo   - idf.py flash      (upload to device)
echo   - idf.py monitor    (view logs)
echo.
echo Press any key to continue...
pause >nul
