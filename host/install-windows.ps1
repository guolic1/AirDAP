param(
    [Parameter(Mandatory)][ValidateSet('Install','Start','Stop','Status','Remove')][string]$Action,
    [ValidatePattern('^[A-Za-z][A-Za-z0-9_-]{0,63}$')][string]$Name = 'AirDAP',
    [string]$InstallDirectory = "$env:ProgramFiles\AirDAP",
    [string]$DataDirectory = "$env:ProgramData\AirDAP",
    [string]$PythonPath,
    [string]$IdfPath,
    [string]$ProvisioningDirectory,
    [ValidateRange(1,65535)][int]$HttpPort = 8080,
    [ValidateRange(1,65535)][int]$UsbipPort = 3242,
    [switch]$NoHttp
)
$ErrorActionPreference = 'Stop'
$description = 'AirDAP local USB/IP and device management service'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if ($Action -ne 'Status' -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '请在管理员 PowerShell 中运行此脚本。'
}
$existing = Get-CimInstance Win32_Service -Filter "Name='$Name'"
if ($existing -and $existing.Description -ne $description) { throw '同名服务不属于 AirDAP，拒绝修改。' }
if ($Action -eq 'Status') {
    if ($existing) { $existing | Select-Object Name, State, StartMode, PathName } else { Write-Output 'AirDAP 服务尚未安装。' }
    exit 0
}
if ($Action -ne 'Install') {
    if (-not $existing) { throw 'AirDAP 服务尚未安装。' }
    switch ($Action) {
        'Start' { Start-Service -Name $Name }
        'Stop' { Stop-Service -Name $Name; (Get-Service $Name).WaitForStatus('Stopped', [TimeSpan]::FromMinutes(10)) }
        'Remove' {
            Stop-Service -Name $Name
            (Get-Service $Name).WaitForStatus('Stopped', [TimeSpan]::FromMinutes(10))
            & sc.exe delete $Name
            if ($LASTEXITCODE -ne 0) { throw '删除服务注册失败。' }
            Write-Output '服务注册已移除；程序、配置和凭据文件已保留。'
        }
    }
    exit 0
}
if ($existing) { throw '服务已经安装。请先停止并移除旧服务注册，再选择新的安装目录。' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$install = [IO.Path]::GetFullPath($InstallDirectory)
$data = [IO.Path]::GetFullPath($DataDirectory)
$programRoot = [IO.Path]::GetFullPath($env:ProgramFiles).TrimEnd('\') + '\'
$dataRoot = [IO.Path]::GetFullPath($env:ProgramData).TrimEnd('\') + '\'
if (-not $install.StartsWith($programRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not $data.StartsWith($dataRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw '特权服务程序必须安装到 Program Files 子目录，数据必须使用 ProgramData 子目录。'
}
if (Test-Path -LiteralPath $install) { throw '安装目录已存在，请选择一个新的专用目录。' }
if ((Test-Path -LiteralPath $data) -and -not (Test-Path -LiteralPath (Join-Path $data 'airdap-service-data'))) {
    throw '已有数据目录缺少 AirDAP 标记，拒绝修改其 ACL。请选择新的专用目录。'
}
if ($install.Contains('"') -or $data.Contains('"')) { throw '目录不能包含双引号。' }
if (-not $PythonPath) {
    $PythonPath = (& uv run --project $repo --locked python -c 'import sys; print(sys.executable)').Trim()
    if ($LASTEXITCODE -ne 0) { throw '无法通过 uv 找到 Python。' }
}
$runtimeJson = & $PythonPath -c 'import json,sys,sysconfig,ssl; assert hasattr(ssl.SSLContext,"set_psk_client_callback"); print(json.dumps({"base":sys.base_prefix,"packages":sysconfig.get_path("purelib")}))'
if ($LASTEXITCODE -ne 0) { throw '需要安装项目依赖的 Python 3.13+ 环境。' }
$runtime = $runtimeJson | ConvertFrom-Json
New-Item -ItemType Directory -Path $install | Out-Null
New-Item -ItemType Directory -Force -Path $data | Out-Null
# Service code, interpreter and secrets must not inherit ordinary-user write access.
foreach ($path in @($install, $data)) {
    & icacls.exe $path /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '无法保护服务目录 ACL。' }
}
Set-Content -LiteralPath (Join-Path $data 'airdap-service-data') -Value 'AirDAP service data v1'
Copy-Item -LiteralPath $runtime.base -Destination (Join-Path $install 'runtime') -Recurse
$packages = Join-Path $install 'runtime/Lib/site-packages'
New-Item -ItemType Directory -Force -Path $packages | Out-Null
Get-ChildItem -LiteralPath $runtime.packages | Copy-Item -Destination $packages -Recurse -Force
$hostDir = Join-Path $install 'host'
$toolsDir = Join-Path $install 'firmware/tools'
New-Item -ItemType Directory -Force -Path $hostDir, $toolsDir | Out-Null
Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.py' -File | Copy-Item -Destination $hostDir
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'web') -Destination (Join-Path $hostDir 'web') -Recurse
Get-ChildItem -LiteralPath (Join-Path $repo 'firmware/tools') -Filter '*.py' -File | Copy-Item -Destination $toolsDir
$extra = ''
$idfFile = Join-Path $repo 'firmware/.airdap-env/idf-path.txt'
$idf = $IdfPath
if (-not $idf) { $idf = $env:IDF_PATH }
if (-not $idf -and (Test-Path -LiteralPath $idfFile)) { $idf = (Get-Content -LiteralPath $idfFile -TotalCount 1) }
$prov = $ProvisioningDirectory
if (-not $prov) { $prov = Join-Path $repo 'firmware/managed_components/espressif__network_provisioning/tool/esp_prov' }
if ($idf -and (Test-Path -LiteralPath $prov)) {
    $proto = Join-Path $install 'provisioning/idf/components/protocomm'
    New-Item -ItemType Directory -Force -Path $proto | Out-Null
    Copy-Item -LiteralPath (Join-Path $idf 'components/protocomm/python') -Destination (Join-Path $proto 'python') -Recurse
    Copy-Item -LiteralPath $prov -Destination (Join-Path $install 'provisioning/esp_prov') -Recurse
    $extra = ' --idf-path "' + (Join-Path $install 'provisioning/idf') + '" --provisioning-dir "' + (Join-Path $install 'provisioning/esp_prov') + '"'
} else { Write-Warning '未发现 ESP-IDF 配网组件。USB 配网、网络桥接和 OTA 可用；蓝牙配网需补齐组件。' }
$python = Join-Path $install 'runtime/python.exe'
& $python -c 'import ssl,bleak,cryptography,google.protobuf,usb.core,libusb_package; assert hasattr(ssl.SSLContext,"set_psk_client_callback")'
if ($LASTEXITCODE -ne 0) { throw '复制的运行环境验证失败，未注册服务。' }
$entry = Join-Path $hostDir 'airdap-service.py'
$binary = '"' + $python + '" "' + $entry + '" --windows-service --service-name ' + $Name + ' --data-dir "' + $data + '" --http-port ' + $HttpPort + ' --usbip-port ' + $UsbipPort + $extra
if ($NoHttp) { $binary += ' --no-http' }
New-Service -Name $Name -DisplayName 'AirDAP Device Service' -BinaryPathName $binary -Description $description -StartupType Automatic | Out-Null
& sc.exe failure $Name reset= 86400 actions= restart/10000/restart/30000/restart/60000
if ($LASTEXITCODE -ne 0) { throw '服务已注册，但配置失败恢复策略失败。' }
& sc.exe failureflag $Name 1
if ($LASTEXITCODE -ne 0) { throw '服务已注册，但配置异常退出恢复失败。' }
Start-Service -Name $Name
Write-Output "服务已安装并启动。管理页：http://127.0.0.1:$HttpPort；数据：$data"
