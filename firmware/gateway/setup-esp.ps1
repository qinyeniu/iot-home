# ESP-IDF v5.5.4 Terminal Setup Script
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  ESP-IDF v5.5.4 Terminal Setup" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

Write-Host "Setting up ESP-IDF environment..." -ForegroundColor Yellow
cd C:\Espressif\v5.5.4\esp-idf
.\export.ps1

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  ESP-IDF environment ready!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "You can now use idf.py commands:" -ForegroundColor White
Write-Host "  - idf.py build      (compile firmware)" -ForegroundColor Gray
Write-Host "  - idf.py flash      (upload to device)" -ForegroundColor Gray
Write-Host "  - idf.py monitor    (view logs)" -ForegroundColor Gray
Write-Host ""
