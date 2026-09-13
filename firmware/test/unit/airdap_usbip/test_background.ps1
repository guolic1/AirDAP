param([Parameter(Mandatory = $true)][string]$PythonPath)
$ErrorActionPreference = 'Stop'
$testState = @{task = $null; started = 0; stopped = 0; removed = 0;
    actionDefinition = $null; principalDefinition = $null}

# Isolate OS registration; only the real interpreter's TLS capability check runs.
function uv { $PythonPath }
function Get-ScheduledTask { $testState.task }
function New-ScheduledTaskAction {
    param($Execute, $Argument, $WorkingDirectory)
    $testState.actionDefinition = $PSBoundParameters
    'action'
}
function New-ScheduledTaskPrincipal {
    param($UserId, $LogonType, $RunLevel)
    $testState.principalDefinition = $PSBoundParameters
    'principal'
}
function New-ScheduledTaskTrigger { 'trigger' }
function New-ScheduledTaskSettingsSet { 'settings' }
function Register-ScheduledTask {
    param($TaskName, $Description, $Action, $Trigger, $Principal, $Settings)
    $testState.task = [pscustomobject]@{TaskName = $TaskName; Description = $Description; State = 'Ready'}
}
function Start-ScheduledTask { $testState.started++ }
function Stop-ScheduledTask { $testState.stopped++ }
function Unregister-ScheduledTask { $testState.removed++; $testState.task = $null }
function Get-ScheduledTaskInfo { [pscustomobject]@{LastTaskResult = 0} }
function Assert($Condition, $Message) {
    if (-not $Condition) { throw $Message }
}
function Expect-Failure($Operation, $Message) {
    $failed = $false
    try { & $Operation | Out-Null } catch { $failed = $true }
    Assert $failed $Message
}

$tool = Join-Path $PSScriptRoot '../../../tools/airdap-usbip-task.ps1'
$testDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('airdap usbip ' + [guid]::NewGuid())
$previousAppData = $env:LOCALAPPDATA
try {
    New-Item -ItemType Directory -Path $testDirectory | Out-Null
    $env:LOCALAPPDATA = $testDirectory
    $credential = Join-Path $testDirectory 'test credential.json'
    Set-Content -LiteralPath $credential -Value '{}' # registration only; no credential is loaded
    $global:LASTEXITCODE = 0
    & $tool -Action Install -DeviceHost airdap.test -Credential $credential -Port 3242 | Out-Null
    Assert ($testState.started -eq 1) 'install did not start the task'
    Assert ($testState.actionDefinition.Execute.EndsWith('pythonw.exe')) 'background task can open a console'
    Assert ($testState.actionDefinition.Argument.Contains('"' + $credential + '"')) 'credential path with spaces was not quoted'
    Assert ($testState.actionDefinition.Argument.Contains('"--port" "3242"')) 'port setting was lost'
    Assert ($testState.principalDefinition.LogonType -eq 'Interactive') 'unexpected login scope'
    Assert ($testState.principalDefinition.RunLevel -eq 'Limited') 'unexpected privilege escalation'
    Expect-Failure { & $tool -Action Install -DeviceHost airdap.test -Credential $credential } 'install overwrote an existing task'
    & $tool -Action Status | Out-Null
    & $tool -Action Stop
    & $tool -Action Start
    Assert ($testState.started -eq 2 -and $testState.stopped -eq 1) 'start/stop did not target the task'
    $testState.task.Description = 'unrelated task'
    Expect-Failure { & $tool -Action Remove } 'removed an unrelated task'
    Assert ($testState.removed -eq 0) 'unrelated task removal had side effects'
    $testState.task.Description = 'AirDAP local USB/IP bridge for this user'
    & $tool -Action Remove
    Assert ($testState.removed -eq 1 -and $testState.stopped -eq 2) 'remove did not stop and unregister'
    Assert (Test-Path -LiteralPath $credential) 'remove deleted credentials'
    Expect-Failure { & $tool -Action Start } 'start succeeded without an installed task'
    Write-Output 'PASS: background task registration, quoting, lifecycle and ownership guards'
} finally {
    $env:LOCALAPPDATA = $previousAppData
    # Delete only the precise unique test directory under the system temp root.
    $resolved = [System.IO.Path]::GetFullPath($testDirectory)
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a test directory outside the temp root.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
