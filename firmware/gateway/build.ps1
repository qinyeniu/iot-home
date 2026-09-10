# Build script for IoT-Home Gateway
Write-Host "Building IoT-Home Gateway..." -ForegroundColor Cyan

# Set environment
$env:IDF_PATH = "C:\Espressif\v5.5.4\esp-idf"
$env:PATH = "C:\Espressif\tools\python\v5.5.4\venv\Scripts;C:\Espressif\tools\idf-python\3.11.2\Scripts;$env:PATH"

# Navigate to project
cd "C:\Users\HJB\Documents\iot-home\firmware\gateway"

# Build
Write-Host "Running idf.py build..." -ForegroundColor Yellow
& "C:\Espressif\v5.5.4\esp-idf\tools\idf.py" build

if ($LASTEXITCODE -eq 0) {
    Write-Host "Build successful!" -ForegroundColor Green
} else {
    Write-Host "Build failed!" -ForegroundColor Red
}
