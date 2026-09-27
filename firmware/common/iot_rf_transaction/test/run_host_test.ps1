$ErrorActionPreference = "Stop"
$componentRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $componentRoot "build-host-ninja"
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
cmake -G Ninja -S (Join-Path $componentRoot "test") -B $buildRoot
cmake --build $buildRoot
ctest --test-dir $buildRoot --output-on-failure
