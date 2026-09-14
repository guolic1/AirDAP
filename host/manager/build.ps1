#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$ServiceBinary = "$PSScriptRoot\..\target\release\airdap-service.exe",
    [string]$OutputDirectory = "$PSScriptRoot\..\..\build\releases\windows-x64",
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Windows x64 with .NET Framework 4.x is required.' }
$source = (Resolve-Path -LiteralPath $ServiceBinary).Path
$version = & $source --version
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^airdap-service ') { throw 'Invalid AirDAP service executable.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$common = @('/nologo','/optimize+','/warnaserror+','/platform:x64','/langversion:5',
    '/reference:System.Management.dll','/reference:System.ServiceProcess.dll',
    '/reference:System.Windows.Forms.dll','/reference:System.Drawing.dll',
    "/resource:$source,AirDAP.Service.exe", "$PSScriptRoot\ServiceManager.cs", "$PSScriptRoot\ManagerWindow.cs")
& $compiler /target:winexe "/win32manifest:$PSScriptRoot\app.manifest" "/out:$output\AirDAP-Manager.exe" @common
if ($LASTEXITCODE -ne 0) { throw 'Manager compilation failed.' }
if ($Test) {
    & $compiler /target:exe /main:AirDAP.Manager.ManagerTests "/out:$output\AirDAP-Manager.Tests.exe" @common "$PSScriptRoot\ManagerTests.cs"
    if ($LASTEXITCODE -ne 0) { throw 'Manager test compilation failed.' }
    & "$output\AirDAP-Manager.Tests.exe" "$output\manager-preview.png"
    if ($LASTEXITCODE -ne 0) { throw 'Manager tests failed.' }
    & $compiler /nologo /target:exe /platform:x64 /main:AirDAP.Manager.FailureFixture "/out:$output\AirDAP-FailureFixture.exe" "$PSScriptRoot\FailureFixture.cs"
    if ($LASTEXITCODE -ne 0) { throw 'Failure fixture compilation failed.' }
    $failureOptions = @($common | Where-Object { $_ -notlike '/resource:*' })
    & $compiler /target:exe /define:FAILURE_PROBE /main:AirDAP.Manager.FailureProbe "/out:$output\AirDAP-FailureProbe.exe" "/resource:$output\AirDAP-FailureFixture.exe,AirDAP.Service.exe" @failureOptions "$PSScriptRoot\FailureFixture.cs"
    if ($LASTEXITCODE -ne 0) { throw 'Failure probe compilation failed.' }
}
Write-Output "Built: $output\AirDAP-Manager.exe ($version)"
