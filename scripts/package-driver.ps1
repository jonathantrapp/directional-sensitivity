param([Parameter(Mandatory=$true)][string]$HardwareId)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
# Accept a concrete HID hardware ID, not a compatible/class wildcard or INF
# syntax. Device instance paths from raw_capture/driver_ctl are not hardware IDs.
if ($HardwareId -notmatch '^HID\\VID_[0-9A-Fa-f]{4}&PID_[0-9A-Fa-f]{4}(?:&[A-Za-z0-9_]+)*$') {
    throw 'Supply an exact HID VID/PID hardware ID from Device Manager, without wildcards.'
}
$binary = Join-Path $projectRoot 'build\driver\Release\directional_filter.sys'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Build the driver first using scripts/build-driver.ps1.' }
$packageRoot = Join-Path $projectRoot 'build\driver-package'
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
$template = [System.IO.File]::ReadAllText((Join-Path $projectRoot 'driver\DirectionalSensitivity.inf.in'))
$infPath = Join-Path $packageRoot 'DirectionalSensitivity.inf'
[System.IO.File]::WriteAllText($infPath,$template.Replace('@HARDWARE_ID@',$HardwareId),[System.Text.Encoding]::ASCII)
Copy-Item -LiteralPath $binary -Destination (Join-Path $packageRoot 'directional_filter.sys') -Force
$verifier = Join-Path $projectRoot 'build\wdk\package\c\tools\10.0.26100.0\x64\infverif.exe'
& $verifier /w $infPath
if ($LASTEXITCODE) { throw 'INF validation failed.' }
Write-Output "Prepared unsigned package at $packageRoot. Signing and isolated-machine testing are still required."
