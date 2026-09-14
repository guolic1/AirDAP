#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [ValidateSet('install','start','stop','status','remove')][string]$Action = 'status',
    [string]$Binary = "$PSScriptRoot\target\release\airdap-service.exe",
    [ValidateRange(1,65535)][int]$HttpPort = 8080,
    [ValidateRange(1,65535)][int]$UsbipPort = 3242,
    [switch]$NoHttp
)
$ErrorActionPreference = 'Stop'
$serviceName = 'AirDAPNative'
$program = Join-Path $env:ProgramFiles 'AirDAPNative'
$data = Join-Path $env:ProgramData 'AirDAPNative'
$exe = Join-Path $program 'airdap-service.exe'
$existing = Get-CimInstance Win32_Service -Filter "Name='$serviceName'"
if ($existing -and -not $existing.PathName.StartsWith('"' + $exe + '" --windows-service ', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Existing service does not belong to this installer.'
}
if ($Action -ne 'install') {
    if (-not $existing) { throw 'Native service is not installed.' }
    switch ($Action) {
        'start' { Start-Service -Name $serviceName }
        'stop' { Stop-Service -Name $serviceName }
        'status' { Get-Service -Name $serviceName }
        'remove' {
            Stop-Service -Name $serviceName
            & sc.exe delete $serviceName
            if ($LASTEXITCODE -ne 0) { throw 'Service deletion failed.' }
            Write-Output 'Registration removed; program and credentials retained.'
        }
    }
    return
}
if (-not $NoHttp -and $HttpPort -eq $UsbipPort) { throw 'HTTP and USB/IP ports must differ.' }
if ($existing -or (Test-Path -LiteralPath $program) -or (Test-Path -LiteralPath $data)) { throw 'Existing native installation or data found; inspect it before replacing files.' }
$source = (Resolve-Path -LiteralPath $Binary).Path
& $source --version
if ($LASTEXITCODE -ne 0) { throw 'Executable validation failed.' }
# Protected program/data paths prevent writable service binaries and credential disclosure.
New-Item -ItemType Directory -Path $program -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $data -Force | Out-Null
foreach ($path in @($program,$data)) {
    & icacls.exe $path '/inheritance:r' '/grant:r' '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F'
    if ($LASTEXITCODE -ne 0) { throw 'Directory ACL protection failed.' }
}
Copy-Item -LiteralPath $source -Destination $exe
$arguments = '"' + $exe + '" --windows-service --service-name ' + $serviceName + ' --data-dir "' + $data + '" --http-port ' + $HttpPort + ' --usbip-port ' + $UsbipPort
if ($NoHttp) { $arguments += ' --no-http' }
New-Service -Name $serviceName -DisplayName 'AirDAP Native' -BinaryPathName $arguments -StartupType Automatic -Description 'AirDAP native USB/IP and local device management'
& sc.exe failure $serviceName reset= 86400 actions= restart/5000/restart/15000/restart/60000
if ($LASTEXITCODE -ne 0) { throw 'Service recovery configuration failed.' }
Start-Service -Name $serviceName
if (-not $NoHttp) { Write-Output "AirDAP native enabled: http://airdap.localhost:$HttpPort" }
