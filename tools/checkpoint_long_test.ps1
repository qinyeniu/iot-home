# Generate read-only long-test checkpoints. This script never opens a serial port.
param(
    [Parameter(Mandatory=$true)] [string]$NodeLog,
    [Parameter(Mandatory=$true)] [string]$GatewayLog,
    [Parameter(Mandatory=$true)] [string]$StartTime,
    [Parameter(Mandatory=$true)] [string]$OutputDir,
    [int[]]$OffsetsSeconds = @(3600, 7200, 10800, 14400),
    [int]$PostDelaySeconds = 15,
    [string]$RepoRoot = (Get-Location),
    [string]$PythonExecutable = ""
)

$ErrorActionPreference = 'Stop'

$time_format = "yyyy-MM-dd'T'HH:mm:ss"
$parsed_start = [datetime]::ParseExact($StartTime, $time_format,
    [Globalization.CultureInfo]::InvariantCulture)

$resolved_repo = (Resolve-Path -LiteralPath $RepoRoot).Path
$resolved_node = (Resolve-Path -LiteralPath $NodeLog).Path
$resolved_gateway = (Resolve-Path -LiteralPath $GatewayLog).Path
if ([string]::IsNullOrWhiteSpace($PythonExecutable)) {
    $default_python = Join-Path $resolved_repo 'server/backend/.venv/Scripts/python.exe'
    $PythonExecutable = if (Test-Path -LiteralPath $default_python) { $default_python } else { 'python' }
}
if (-not (& $PythonExecutable --version)) {
    throw "Python executable is not usable: $PythonExecutable"
}

Push-Location -LiteralPath $resolved_repo
try {
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    $resolved_output = (Resolve-Path -LiteralPath $OutputDir).Path

    foreach ($offset in $OffsetsSeconds) {
        $runAt = $parsed_start.AddSeconds($offset + $PostDelaySeconds)
        $wait = [int]($runAt - (Get-Date)).TotalSeconds
        while ($wait -gt 0) {
            $sleep = [Math]::Min(30, $wait)
            [System.Threading.Thread]::Sleep([int]$sleep * 1000)
            $wait = [int]($runAt - (Get-Date)).TotalSeconds
        }

        $json = Join-Path $resolved_output ("checkpoint-{0}s.json" -f $offset)
        $markdown = Join-Path $resolved_output ("checkpoint-{0}s.md" -f $offset)
        & $PythonExecutable tools/analyze_long_test.py --target-duration $offset `
            --json $json --markdown $markdown $resolved_node $resolved_gateway

        if ($LASTEXITCODE -ne 0) {
            throw "analyze_long_test.py failed at checkpoint ${offset}s (exit code $LASTEXITCODE)"
        }

        "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] checkpoint $offset -> $markdown" |
            Tee-Object -FilePath (Join-Path $resolved_output 'checkpoints.log') -Append
    }
}
finally {
    Pop-Location
}