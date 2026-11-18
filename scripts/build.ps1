$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath)) { throw 'Install Visual Studio C++ Build Tools and CMake first.' }
$installation = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools were not found.' }
$cmakePath = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmakePath)) {
    $cmakePath = (Get-Command cmake -ErrorAction Stop).Source
}
Push-Location $projectRoot
try {
    & $cmakePath -S . -B build -G 'Visual Studio 18 2026' -A x64
    if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
    & $cmakePath --build build --config Release
    if ($LASTEXITCODE) { throw 'Compilation failed.' }
    & (Join-Path (Split-Path $cmakePath) 'ctest.exe') --test-dir build -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed.' }
} finally { Pop-Location }
