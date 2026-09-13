[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'Start', 'Stop', 'Status', 'Remove')]
    [string]$Action,
    [ValidatePattern('^[A-Za-z0-9._:-]+$')]
    [string]$DeviceHost,
    [string]$Credential,
    [ValidateRange(1, 65535)]
    [int]$Port = 3240
)

$ErrorActionPreference = 'Stop'
$taskUser = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$taskName = "AirDAP-USBIP-$($taskUser.User.Value)"
$description = 'AirDAP local USB/IP bridge for this user'
$task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($task -and $task.Description -ne $description) {
    throw "Task $taskName exists with a different purpose; refusing to modify it."
}

if ($Action -eq 'Install') {
    if ($task) {
        throw 'The AirDAP task already exists. Remove it before installing a new configuration.'
    }
    if (-not $DeviceHost -or -not $Credential) {
        throw 'Install requires -DeviceHost and -Credential.'
    }
    $repoPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
    $credentialPath = (Resolve-Path -LiteralPath $Credential).Path
    # Resolve the repository-managed interpreter once; logon does not run uv or
    # depend on PATH, a console window, or a network dependency installation.
    $pythonPath = & uv run --project $repoPath --locked python -c 'import sys; print(sys.executable)'
    if ($LASTEXITCODE -ne 0) { throw 'Unable to resolve the uv Python environment.' }
    $pythonPath = $pythonPath.Trim()
    $pythonwPath = Join-Path (Split-Path -Parent $pythonPath) 'pythonw.exe'
    if (-not (Test-Path -LiteralPath $pythonwPath -PathType Leaf)) {
        throw "Windowless interpreter not found: $pythonwPath"
    }
    & $pythonPath -c 'import ssl; assert hasattr(ssl.SSLContext, "set_psk_client_callback")'
    if ($LASTEXITCODE -ne 0) { throw 'Python must support TLS-PSK.' }
    $logDirectory = Join-Path $env:LOCALAPPDATA 'AirDAP'
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $logPath = Join-Path $logDirectory 'usbip.log'
    $arguments = @((Join-Path $PSScriptRoot 'airdap-usbip.py'), $DeviceHost,
        '--credential', $credentialPath, '--port', "$Port", '--log-file', $logPath)
    # All values are hostnames or file paths. Reject unsupported quotes/newlines
    # and trailing backslashes rather than constructing ambiguous CRT arguments.
    $quoted = foreach ($argument in $arguments) {
        if ($argument -match '["\r\n]' -or $argument.EndsWith('\')) {
            throw 'An argument cannot be safely represented in the task command line.'
        }
        '"' + $argument + '"'
    }
    $taskAction = New-ScheduledTaskAction -Execute $pythonwPath `
        -Argument ($quoted -join ' ') -WorkingDirectory $repoPath
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $taskUser.Name
    $principal = New-ScheduledTaskPrincipal -UserId $taskUser.Name `
        -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew `
        -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 `
        -RestartInterval (New-TimeSpan -Minutes 1) -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -StartWhenAvailable
    Register-ScheduledTask -TaskName $taskName -Description $description `
        -Action $taskAction -Trigger $trigger -Principal $principal -Settings $settings | Out-Null
    Start-ScheduledTask -TaskName $taskName
    Write-Output "Installed and started $taskName. Log: $logPath"
    Write-Output "After startup, attach bus 1-1 with the USB/IP client on 127.0.0.1:$Port."
    return
}

if (-not $task) { throw "AirDAP task is not installed: $taskName" }
switch ($Action) {
    'Start' { Start-ScheduledTask -TaskName $taskName }
    'Stop' { Stop-ScheduledTask -TaskName $taskName }
    'Status' {
        $task | Select-Object TaskName, State
        Get-ScheduledTaskInfo -TaskName $taskName |
            Select-Object LastRunTime, LastTaskResult, NextRunTime
    }
    'Remove' {
        Stop-ScheduledTask -TaskName $taskName
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    }
}
