# 修复 Codex 个人资料错误
$configPath = "C:\Users\HJB\.codex\config.toml"
$backupPath = "C:\Users\HJB\.codex\config.toml.bak"
Copy-Item -Path $configPath -Destination $backupPath -Force
Write-Host "已备份原配置到: $backupPath"
$content = Get-Content $configPath -Raw
$disableProfileLine = 'CLAUDE_CODE_DISABLE_PROFILE_FETCH = "1"'
if ($content -notmatch "CLAUDE_CODE_DISABLE_PROFILE_FETCH") {
    $content = $content -replace '(\[shell_environment_policy\.set\])', "`r`n$disableProfileLine`r`n`$1"
    Set-Content -Path $configPath -Value $content -Force
    Write-Host "已添加 CLAUDE_CODE_DISABLE_PROFILE_FETCH = 1"
} else {
    Write-Host "CLAUDE_CODE_DISABLE_PROFILE_FETCH 已存在"
}
Write-Host "修复完成！请重新启动 Codex。"
