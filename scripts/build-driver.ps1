param([switch]$DownloadWdk)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$wdkRoot = Join-Path $projectRoot 'build\wdk'
$wdkPackage = Join-Path $wdkRoot 'wdk.nupkg'
$wdkHeaders = Join-Path $wdkRoot 'package\c\Include\10.0.26100.0\km\ntddk.h'
$expectedHash = 'C393D03DFB640B5C92F546B32F6770EF68CD3AAF691956E7D66D8E2C28A1B55E'
if (-not (Test-Path -LiteralPath $wdkHeaders)) {
    if (-not $DownloadWdk) { throw 'WDK missing. Run scripts/build-driver.ps1 -DownloadWdk to fetch the pinned Microsoft package.' }
    New-Item -ItemType Directory -Path $wdkRoot -Force | Out-Null
    if (-not (Test-Path -LiteralPath $wdkPackage)) {
        Invoke-WebRequest -Uri 'https://www.nuget.org/api/v2/package/Microsoft.Windows.WDK.x64/10.0.26100.6584' -OutFile $wdkPackage -TimeoutSec 120
    }
    if ((Get-FileHash -LiteralPath $wdkPackage -Algorithm SHA256).Hash -ne $expectedHash) { throw 'WDK download hash mismatch.' }
    Expand-Archive -LiteralPath $wdkPackage -DestinationPath (Join-Path $wdkRoot 'package') -Force
}
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmakePath = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmakePath -S $projectRoot -B (Join-Path $projectRoot 'build') -G 'Visual Studio 18 2026' -A x64 -DDIRECTIONAL_BUILD_DRIVER=ON
if ($LASTEXITCODE) { throw 'Driver configuration failed.' }
& $cmakePath --build (Join-Path $projectRoot 'build') --config Release --target directional_filter driver_ctl
if ($LASTEXITCODE) { throw 'Driver build failed.' }
Write-Output 'Built an unsigned test driver. No driver has been installed or activated.'
