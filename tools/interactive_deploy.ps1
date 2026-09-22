$ErrorActionPreference = "Stop"
Set-Location "C:\Users\HJB\Documents\iot-home"
$logPath = "C:\Users\HJB\Documents\iot-home\tools\deploy_server_20260922.log"
"部署开始：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" | Set-Content $logPath -Encoding UTF8
try {
    $secure = Read-Host "请输入服务器 root 密码（输入时不会显示）" -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        $env:IOT_HOME_SSH_PASSWORD = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
        python tools/deploy_server.py --confirm *>&1 | Tee-Object -FilePath $logPath -Append
        if ($LASTEXITCODE -ne 0) { throw "部署失败，退出码：$LASTEXITCODE" }
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
        Remove-Item Env:IOT_HOME_SSH_PASSWORD -ErrorAction SilentlyContinue
    }
    "部署完成：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" | Add-Content $logPath -Encoding UTF8
} catch {
    "部署失败：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')：$($_.Exception.Message)" | Add-Content $logPath -Encoding UTF8
    Write-Host $_.Exception.Message
    Read-Host "按回车关闭窗口"
    exit 1
}
