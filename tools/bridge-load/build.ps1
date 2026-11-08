# Builds bridge-load.exe with the same Visual Studio the mod builds with.
#
#   pwsh -ExecutionPolicy Bypass -File .\build.ps1
#
# cl.exe is found by loading the VS developer environment (vswhere -> Enter-VsDevShell),
# the same way build-local.ps1 does it, so no developer prompt is needed.

[CmdletBinding()]
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$exe = Join-Path $here 'bridge-load.exe'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'No Visual Studio with the C++ x64 toolset found' }

if (-not $env:VSCMD_VER) {
    Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw "'cl' not on PATH after loading the VS environment" }

if ($Clean -and (Test-Path $exe)) { Remove-Item -Force $exe }

Push-Location $here
try {
    & cl /nologo /std:c++20 /O2 /EHsc /W4 /WX /DUNICODE /D_UNICODE main.cpp /Fe:bridge-load.exe
    if ($LASTEXITCODE -ne 0) { throw "cl failed with exit code $LASTEXITCODE" }
}
finally { Pop-Location }

if (-not (Test-Path $exe)) { throw "build reported success but $exe is missing" }
Write-Host "built $exe"
