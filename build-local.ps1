<#
  Builds MGPU Scaling (AAAMGPUScaling.dll: FG-GPU1 frame generation and upscaling in one F4SE
  plugin) locally with the Visual Studio already installed.

  Usage (either shell works; when started from Windows PowerShell 5.1 the script re-runs
  itself under PowerShell 7 if pwsh is installed). Run from the repository root, with overlay/
  beside this script:
      pwsh -ExecutionPolicy Bypass -File D:\projects\mgpu-scaling\build-local.ps1

  Needs only: Visual Studio 2026 (or 2022), including the Build Tools edition, with "Desktop
  development with C++", Git, internet.
  First run: ~20-40 min (vcpkg builds boost/directxtk/... once). Later runs: a few minutes.

  Work happens in -WorkDir (short path on purpose: vcpkg breaks on long paths).
  Result: <WorkDir>\package\Data\... and the same as <WorkDir>\MGPU-Scaling.zip for Vortex/MO2.
#>
param(
    [string]$WorkDir = "D:\fgb",
    [string]$UpstreamSha = "3e237a44269a8dc8cab54aab4ef0aae83c284a43",
    [string]$VcpkgSha = "b472291f295551b7127359ea38fdd2ca092f6f1b",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
# Windows PowerShell 5.1 renders a progress bar for every Invoke-WebRequest chunk,
# which makes the ~250 MB FidelityFX download several times slower.
$ProgressPreference = "SilentlyContinue"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Windows PowerShell 5.1 is supported, but this script is developed and tested on
# PowerShell 7 and the two differ in native-command/error behaviour (5.1 turns a native
# command's stderr into a terminating error, for example). If someone started us with
# `powershell`, hand over to pwsh when it is installed so the caller, an IDE or an agent
# never has to remember which one to use. `powershell.exe` is always 5.1; installing 7
# does not change it, which is why this cannot be fixed by configuration alone.
if ($PSVersionTable.PSEdition -eq 'Desktop' -and $PSCommandPath) {
    $pwsh = (Get-Command pwsh.exe -ErrorAction SilentlyContinue).Source
    if ($pwsh) {
        Write-Host "Windows PowerShell $($PSVersionTable.PSVersion) detected; re-running under PowerShell 7..." -ForegroundColor Yellow
        $forward = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
        foreach ($name in $PSBoundParameters.Keys) {
            $value = $PSBoundParameters[$name]
            if ($value -is [System.Management.Automation.SwitchParameter]) {
                if ($value.IsPresent) { $forward += "-$name" }
            } else {
                $forward += @("-$name", [string]$value)
            }
        }
        if ($args.Count -gt 0) { $forward += $args }
        & $pwsh @forward
        $code = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 1 }
        exit $code
    }
    Write-Warning "PowerShell 7 (pwsh) not found; continuing on Windows PowerShell $($PSVersionTable.PSVersion)."
}

function Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Run($exe, [string[]]$argList) {
    & $exe @argList
    if ($LASTEXITCODE -ne 0) { throw "$exe $($argList -join ' ') failed with exit code $LASTEXITCODE" }
}
function Fetch-Pinned($url, $dir, $sha, [switch]$Recursive) {
    if (-not (Test-Path "$dir\.git")) {
        New-Item -ItemType Directory -Force $dir | Out-Null
        Run git @("-C", $dir, "init", "-q")
        Run git @("-C", $dir, "remote", "add", "origin", $url)
    }
    # --verify --quiet keeps stderr empty when the repo has no HEAD yet. Windows
    # PowerShell 5.1 turns a native command's stderr into a terminating error while
    # $ErrorActionPreference is Stop, so a plain `rev-parse HEAD` aborts the script
    # on a fresh checkout (PowerShell 7 does not, which is why this hid for a while).
    $current = (& git -C $dir rev-parse --verify --quiet HEAD)
    if ($current -ne $sha) {
        Run git @("-C", $dir, "fetch", "--depth", "1", "origin", $sha)
        Run git @("-C", $dir, "checkout", "-q", "--force", "FETCH_HEAD")
    }
    if ($Recursive) { Run git @("-C", $dir, "submodule", "update", "--init", "--recursive", "--depth", "1") }
}

# ---------------------------------------------------------------- Visual Studio
Step "Locating Visual Studio"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio with the C++ x64 toolset found" }
$vsMajor = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Split('.')[0]
$generator = if ($vsMajor -eq "18") { "Visual Studio 18 2026" } else { "Visual Studio 17 2022" }
Write-Host "Visual Studio $vsMajor at $vsPath -> generator '$generator'"

# vcpkg runs vcvarsall for the newest VC\Tools\MSVC directory even if that install is partial (a
# half-installed newer toolset next to a complete one), and then fails with the unhelpful "vcpkg was
# unable to detect the active compiler's information". Fail with the reason instead.
$msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
$newestToolset = Get-ChildItem $msvcRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
if ($newestToolset -and -not (Test-Path (Join-Path $newestToolset.FullName "bin\HostX64\x64\cl.exe"))) {
    throw "The newest MSVC toolset '$($newestToolset.Name)' under $msvcRoot is incomplete (no bin\HostX64\x64\cl.exe). vcpkg picks the newest toolset, so it cannot find a compiler. Complete/repair Visual Studio, or move that folder out of VC\Tools\MSVC, then run again."
}

if (-not $env:VSCMD_VER) {
    Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}
foreach ($tool in "git", "cmake", "cl") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "'$tool' not on PATH after loading the VS environment" }
}
Write-Host (cmake --version | Select-Object -First 1)

# ---------------------------------------------------------------- sources
if ($Clean -and (Test-Path "$WorkDir\FO4Upscaling\build")) { Remove-Item -Recurse -Force "$WorkDir\FO4Upscaling\build" }

Step "FO4Upscaling @ $($UpstreamSha.Substring(0,7)) (with submodules)"
$src = "$WorkDir\FO4Upscaling"
Fetch-Pinned "https://github.com/northaxosky/FO4Upscaling.git" $src $UpstreamSha -Recursive

Step "Applying the GPU1 overlay"
# What the last build left in the checkout for each overlay file, to tell which ones change now.
$overlayRoot = (Resolve-Path "$Here\overlay").Path
$overlayFiles = @(Get-ChildItem -Recurse -File $overlayRoot | ForEach-Object {
    [pscustomobject]@{ Source = $_.FullName; Target = Join-Path $src $_.FullName.Substring($overlayRoot.Length + 1) }
})
$previous = @{}
foreach ($file in $overlayFiles) {
    if (Test-Path $file.Target) { $previous[$file.Target] = (Get-FileHash $file.Target -Algorithm SHA256).Hash }
}
# Back to the pinned commit first, so files the overlay no longer carries (it moved or dropped
# some) do not linger from earlier builds. Only the source folders; build\ and extern\ stay.
# reset first: regenerating fg-gpu1.patch marks new files with `git add -N`, and checkout and
# clean leave such intent-to-add files in place, where the CMake glob would compile them.
Run git @("-C", $src, "reset", "-q")
Run git @("-C", $src, "checkout", "-q", "--", ".")
Run git @("-C", $src, "clean", "-fdq", "--", "plugins", "package", "include", "cmake")
Copy-Item -Recurse -Force "$Here\overlay\*" $src
# Copy-Item keeps the overlay files' timestamps. A file edited while the previous build ran is then
# older than its object file and would not be recompiled, so every file whose content changed
# since the last build is stamped now; unchanged ones keep their time and are not rebuilt.
$now = Get-Date
$touched = 0
foreach ($file in $overlayFiles) {
    if ($previous[$file.Target] -ne (Get-FileHash $file.Source -Algorithm SHA256).Hash) {
        (Get-Item $file.Target).LastWriteTime = $now
        $touched++
    }
}
Write-Host "$touched overlay file(s) changed since the last build"
& git -C $src status --short

Step "vcpkg @ $($VcpkgSha.Substring(0,7))"
$vcpkg = "$WorkDir\vcpkg"
Fetch-Pinned "https://github.com/microsoft/vcpkg.git" $vcpkg $VcpkgSha
if (-not (Test-Path "$vcpkg\vcpkg.exe")) { Run "$vcpkg\bootstrap-vcpkg.bat" @("-disableMetrics") }
$env:VCPKG_ROOT = $vcpkg

# ---------------------------------------------------------------- build
Step "Configure"
Copy-Item -Force "$src\cmake\CommonLibF4.cmake.txt" "$src\extern\CommonLibF4\CMakeLists.txt"
Push-Location $src
try {
    Run cmake @("-S", ".", "--preset=default", "-G", $generator)
    # One target: the frame-generation sources (plugins\FrameGeneration\src) and the upscaler
    # (plugins\MGPUScaling\src\upscaler) in one DLL, AAAMGPUScaling.dll.
    Step "Build MGPUScaling (Release)"
    Run cmake @("--build", "build", "--config", "Release", "--target", "MGPUScaling")
} finally { Pop-Location }

# ---------------------------------------------------------------- package
# Everything in its own places (Data\F4SE\Plugins\MGPUScaling, MCM\Config\MGPUScaling), so the
# package shares no file with doodlum's Upscaling or FO4Upscaling's frame generation mod.
Step "Package"
$pkg = "$WorkDir\package"
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
$plugins = "$pkg\Data\F4SE\Plugins"
$data = "$plugins\MGPUScaling"
New-Item -ItemType Directory -Force "$data\Shaders", "$data\FidelityFX", "$pkg\Data\MCM\Config\MGPUScaling" | Out-Null
$bin = "$src\build\plugins\MGPUScaling\Release"
Copy-Item "$bin\AAAMGPUScaling.dll" $plugins
if (Test-Path "$bin\AAAMGPUScaling.pdb") { Copy-Item "$bin\AAAMGPUScaling.pdb" $plugins }
# Frame generation's and the upscaler's shaders (no file name is used twice). No Streamline
# runtime: the upscaler's DLSS method falls back to FSR 3.1 without it; FSR 4.1 does not use it.
Copy-Item "$src\package\F4SE\Plugins\FrameGeneration\*.hlsl", "$src\package\F4SE\Plugins\Upscaling\*.hlsl" "$data\Shaders"
Copy-Item "$src\package\F4SE\Plugins\FrameGeneration\FidelityFX\LICENSE.txt" "$data\FidelityFX"
Copy-Item -Recurse "$src\package\MCM\Config\MGPUScaling\*" "$pkg\Data\MCM\Config\MGPUScaling"

# FidelityFX runtime (same version/hash as upstream scripts/sdk-manifest.sh)
$ffxZip = "$WorkDir\FidelityFX-SDK-v1.1.4.zip"
$ffxSha = "0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952"
if (-not (Test-Path $ffxZip)) {
    Invoke-WebRequest "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip" -OutFile $ffxZip
}
if ((Get-FileHash $ffxZip -Algorithm SHA256).Hash.ToLower() -ne $ffxSha) { Remove-Item $ffxZip; throw "FidelityFX zip hash mismatch" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($ffxZip)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -like "*PrebuiltSignedDLL/amd_fidelityfx_dx12.dll" } | Select-Object -First 1
    if (-not $entry) { throw "amd_fidelityfx_dx12.dll not in the FidelityFX zip" }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$data\FidelityFX\amd_fidelityfx_dx12.dll", $true)
} finally { $zip.Dispose() }

# FSR SDK 2.4 runtime (iFSRRuntime = 1): the loader, the frame-generation effect DLL and the
# upscaler effect DLL (FSR 4.1 on the second GPU). The v2.4.0 release has no binary asset (its
# source archive is ~250 MB), so the signed DLLs are fetched one by one from the tagged commit and
# checked against pinned hashes. The loader finds the effect DLLs next to itself, so they all go
# into the same FidelityFX folder as the 1.1.4 DLL.
$fsr2Commit = "7539ae8b6568ad29ed78a753404351daf3b5cca7"   # tag v2.4.0
$fsr2Cache = "$WorkDir\fsr-sdk-2.4.0"
New-Item -ItemType Directory -Force $fsr2Cache | Out-Null
$fsr2Dlls = [ordered]@{
    "amd_fidelityfx_loader_dx12.dll"          = "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb"
    "amd_fidelityfx_framegeneration_dx12.dll" = "e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6"
    "amd_fidelityfx_upscaler_dx12.dll"        = "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace"
}
foreach ($name in $fsr2Dlls.Keys) {
    $cached = "$fsr2Cache\$name"
    if (-not (Test-Path $cached)) {
        Invoke-WebRequest "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$fsr2Commit/Kits/FidelityFX/signedbin/$name" -OutFile $cached
    }
    if ((Get-FileHash $cached -Algorithm SHA256).Hash.ToLower() -ne $fsr2Dlls[$name]) { Remove-Item $cached; throw "$name hash mismatch" }
    Copy-Item $cached "$data\FidelityFX\$name"
}
Copy-Item "$src\package\F4SE\Plugins\FrameGeneration\FidelityFX\LICENSE-FSR-SDK-2.4.txt" "$data\FidelityFX"
Copy-Item "$Here\README.md" "$pkg\MGPU-Scaling-README.md"
foreach ($licence in "COPYING", "LICENSE", "EXCEPTIONS.md") {
    if (Test-Path "$src\$licence") { Copy-Item "$src\$licence" $pkg }
}

$zip = "$WorkDir\MGPU-Scaling.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip

Step "Done"
Get-ChildItem -Recurse -File $pkg | ForEach-Object { $_.FullName.Substring($pkg.Length + 1) }
Write-Host "`nInstall: add '$zip' in Vortex (or copy '$pkg\Data' into the game folder)." -ForegroundColor Green
Write-Host "Disable the FG-GPU1, FO4Upscaling and doodlum's Upscaling mods: MGPU Scaling replaces all three." -ForegroundColor Green
