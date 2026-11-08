# Builds p2p-probe.exe with the same Visual Studio the mod builds with.
#
#   pwsh -ExecutionPolicy Bypass -File .\build.ps1
#
# Vulkan headers come from the overlay (overlay\include\vulkan, vendored); the probe loads
# vulkan-1.dll at run time, so no Vulkan SDK is needed.
#
# The --exit-hook / --exit-gpu modes use the mod's detour library from the pinned build tree
# (-DetoursDir, default D:\fgb\FO4Upscaling\include\detours, present after one build-local.ps1 run);
# without it the probe builds without those modes.

[CmdletBinding()]
param([switch]$Clean, [string]$DetoursDir = 'D:\fgb\FO4Upscaling\include\detours')

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$exe = Join-Path $here 'p2p-probe.exe'
$include = Join-Path $here '..\..\overlay\include'

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
    $extra = @()
    $detoursLib = Join-Path $DetoursDir 'Release\detours.lib'
    if ((Test-Path (Join-Path $DetoursDir 'Detours.h')) -and (Test-Path $detoursLib)) {
        $extra = @('/DP2P_EXIT_HOOK', '/I', $DetoursDir, $detoursLib)
        Write-Host "with --exit-hook / --exit-gpu ($DetoursDir)"
    } else {
        Write-Host "without --exit-hook / --exit-gpu ($DetoursDir not found)"
    }
    # /MD: the detour library is built against the DLL runtime.
    & cl /nologo /std:c++20 /O2 /MD /EHsc /W4 /WX /DUNICODE /D_UNICODE /I $include main.cpp @extra /Fe:p2p-probe.exe
    if ($LASTEXITCODE -ne 0) { throw "cl failed with exit code $LASTEXITCODE" }
}
finally { Pop-Location }

if (-not (Test-Path $exe)) { throw "build reported success but $exe is missing" }
Write-Host "built $exe"
