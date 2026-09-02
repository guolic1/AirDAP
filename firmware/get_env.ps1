$airdapFirmwareDir = $PSScriptRoot
$airdapEnvironmentDir = Join-Path $airdapFirmwareDir ".airdap-env"
$airdapConfigFile = Join-Path $airdapEnvironmentDir "idf-path.txt"

if (-not (Test-Path -LiteralPath $airdapConfigFile -PathType Leaf)) {
    throw "get_env.ps1: ESP-IDF is not configured; run tools/setup.py first"
}

$airdapConfigLines = @(Get-Content -LiteralPath $airdapConfigFile -ErrorAction Stop)
if ($airdapConfigLines.Count -ne 1 -or [string]::IsNullOrWhiteSpace($airdapConfigLines[0])) {
    throw "get_env.ps1: $airdapConfigFile must contain exactly one ESP-IDF path"
}

$airdapIdfPath = $airdapConfigLines[0]
$airdapExportScript = Join-Path $airdapIdfPath "export.ps1"
if (-not (Test-Path -LiteralPath $airdapExportScript -PathType Leaf)) {
    throw "get_env.ps1: configured ESP-IDF is missing $airdapExportScript"
}

$airdapIdfPath = (Resolve-Path -LiteralPath $airdapIdfPath -ErrorAction Stop).ProviderPath
$env:IDF_TOOLS_PATH = $airdapEnvironmentDir
Remove-Item Env:IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue

. $airdapExportScript

Remove-Variable airdapFirmwareDir, airdapEnvironmentDir, airdapConfigFile
Remove-Variable airdapConfigLines, airdapIdfPath, airdapExportScript
