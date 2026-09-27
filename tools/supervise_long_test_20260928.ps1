param(
    [int[]]$ProcessIds,
    [string]$Analyzer
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Log = Join-Path $Root "backups/long-test-20260928-default-8h/supervisor.log"

function Write-Log([string]$Message) {
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Add-Content -LiteralPath $Log -Value $line
}

Write-Log "Supervisor started; waiting for PIDs: $($ProcessIds -join ',')"
foreach ($processId in $ProcessIds) {
    while ($true) {
        $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($null -eq $process) {
            Write-Log "Capture process $processId exited"
            break
        }
        Start-Sleep -Seconds 30
    }
}

Start-Sleep -Seconds 3
Write-Log "Starting analyzer: $Analyzer"
Push-Location $Root
try {
    python $Analyzer 2>&1 | Tee-Object -FilePath (Join-Path (Split-Path -Parent $Log) "analyzer-output.log")
    $code = $LASTEXITCODE
    Write-Log "Analyzer exited with code $code"

    if ($code -eq 0) {
        git add -- docs/MORNING_REPORT_2026-09-28.md docs/HANDOFF_2026-09-28.md docs/HANDOFF/_2026-09-28.md backups/long-test-20260928-default-8h/analysis.md
        if (git diff --cached --quiet) {
            Write-Log "No staged changes to commit"
        } else {
            git commit -m "记录夜间长测自动分析结果"
            Write-Log "Committed automatic long-test analysis"
        }
    }
} finally {
    Pop-Location
}
Write-Log "Supervisor finished"
