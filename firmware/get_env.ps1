$airdapFirmwareDir = $PSScriptRoot
$airdapEnvironmentDir = Join-Path $airdapFirmwareDir ".airdap-env"
$airdapConfigFile = Join-Path $airdapEnvironmentDir "idf-path.txt"

if (-not (Test-Path -LiteralPath $airdapConfigFile -PathType Leaf)) {
    throw "get_env.ps1: ESP-IDF is not configured; run tools/setup.py first"
}

$airdapConfigLines = @(Get-Content -LiteralPath $airdapConfigFile -ErrorAction Stop)
if ($airdapConfigLines.Count -eq 1 -and -not [string]::IsNullOrWhiteSpace($airdapConfigLines[0])) {
    # Configurations written before provenance was recorded used one line.
    $airdapIdfMode = "legacy"
} elseif (
    $airdapConfigLines.Count -eq 2 -and
    -not [string]::IsNullOrWhiteSpace($airdapConfigLines[0]) -and
    $airdapConfigLines[1] -in @("managed", "external")
) {
    $airdapIdfMode = $airdapConfigLines[1]
} else {
    throw "get_env.ps1: $airdapConfigFile must contain an ESP-IDF path and managed/external mode"
}

$airdapIdfPath = $airdapConfigLines[0]
$airdapExportScript = Join-Path $airdapIdfPath "export.ps1"
if (-not (Test-Path -LiteralPath $airdapExportScript -PathType Leaf)) {
    throw "get_env.ps1: configured ESP-IDF is missing $airdapExportScript"
}

$airdapIdfPath = (Resolve-Path -LiteralPath $airdapIdfPath -ErrorAction Stop).ProviderPath
$env:IDF_TOOLS_PATH = $airdapEnvironmentDir
Remove-Item Env:IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue
$env:IDF_SKIP_TOOLS_CHECK = "1"

$airdapManagedIdfPath = Join-Path $airdapEnvironmentDir "esp-idf"
$airdapUsesManagedIdf = $airdapIdfMode -eq "managed"
if ($airdapIdfMode -eq "legacy" -and (Test-Path -LiteralPath $airdapManagedIdfPath -PathType Container)) {
    $airdapManagedIdfPath = (
        Resolve-Path -LiteralPath $airdapManagedIdfPath -ErrorAction Stop
    ).ProviderPath
    $airdapUsesManagedIdf = [StringComparer]::OrdinalIgnoreCase.Equals(
        $airdapIdfPath,
        $airdapManagedIdfPath
    )
}
if ($airdapUsesManagedIdf) {
    $env:IDF_SKIP_CHECK_SUBMODULES = "1"
} else {
    Remove-Item Env:IDF_SKIP_CHECK_SUBMODULES -ErrorAction SilentlyContinue
}

. $airdapExportScript

Remove-Variable airdapFirmwareDir, airdapEnvironmentDir, airdapConfigFile
Remove-Variable airdapConfigLines, airdapIdfPath, airdapIdfMode, airdapExportScript
Remove-Variable airdapManagedIdfPath, airdapUsesManagedIdf
