<#
  Records one PresentMon capture of Fallout 4 for the frame-pacing A/B, then prints the summary
  of every capture so far (presentmon-summary.ps1).

  Usage:
      pwsh -File D:\projects\mgpu-scaling\tools\presentmon-capture.ps1 -Label fsr24-sc400

  1. Get the game to the test spot. Alt-tab out and start this.
  2. PresentMon needs admin rights to read the graphics event trace, so Windows asks once (UAC);
     the recording runs in a minimised admin window. Start this from an admin PowerShell to skip
     the prompt.
  3. Go back into the game. After -Delay seconds (default 10) one beep: recording, -Seconds long
     (default 60). Two beeps: done. Do the same thing in the game for every run (stand still, or the
     same walk / camera pan), at the same settings.
  4. Alt-tab back here for the summary.

  Captures go to -OutDir as <date>-<time>-<Label>.csv. Next to each, <...>.config.txt records the FSR
  runtime and swap chain the game was actually using, read from MGPUScaling.log, so a
  capture is never mislabelled by a setting that did not stick.
#>
param(
    [Parameter(Mandatory)][string]$Label,
    [int]$Seconds = 60,
    [int]$Delay = 10,
    [string]$PresentMon = "D:\fgb\presentmon\PresentMon-2.6.0-x64.exe",
    [string]$OutDir = "D:\fgb\presentmon\captures",
    [string]$Process = "Fallout4.exe",
    # Internal: the elevated child records into this file.
    [string]$Csv
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $PresentMon)) { throw "PresentMon not found at $PresentMon" }
if ($Label -notmatch '^[\w.-]+$') { throw "-Label may only contain letters, digits, '.', '-' and '_'" }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

# What the running game really uses, from the plugin's log (truncated at every game start).
# A log older than the game process belongs to an earlier session and describes nothing: the
# plugin was renamed to MGPUScaling, so a stale AAAFrameGeneration.log is still on disk and
# used to be read as if it were this run's configuration.
function Get-FsrConfig {
    $dir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Fallout4\F4SE'
    $exe = [IO.Path]::GetFileNameWithoutExtension($Process)
    $game = Get-Process -Name $exe -ErrorAction SilentlyContinue | Select-Object -First 1

    $log = @('MGPUScaling.log', 'AAAFrameGeneration.log') |
        ForEach-Object { Join-Path $dir $_ } |
        Where-Object { Test-Path $_ } |
        Sort-Object { (Get-Item $_).LastWriteTime } -Descending |
        Select-Object -First 1
    if (-not $log) { return "unknown (no MGPUScaling.log or AAAFrameGeneration.log)" }

    $written = (Get-Item $log).LastWriteTime
    if ($game -and $written -lt $game.StartTime) {
        return ("no frame generation this session ({0} last written {1:HH:mm:ss}, game started {2:HH:mm:ss})" -f `
                [IO.Path]::GetFileName($log), $written, $game.StartTime)
    }

    $lines = @(Get-Content $log -ErrorAction SilentlyContinue)
    $runtime = $lines | Where-Object { $_ -match '\[FSR\] runtime: ' } | Select-Object -Last 1
    if (-not $runtime) {
        if ($game) { return ("no frame generation this session ({0} written {1:HH:mm:ss}, no [FSR] lines)" -f [IO.Path]::GetFileName($log), $written) }
        return "unknown (log has no [FSR] lines - older plugin build?)"
    }
    if ($runtime -match '1\.1\.4') { return "SDK 1.1.4: swap chain 3.1.4 + FG 3.1.4" }
    $inUse = $lines | Where-Object { $_ -match '\[FSR\] in use: swap chain (\S+) \+ frame generation (\S+)' } | Select-Object -Last 1
    if ($inUse -and $inUse -match 'swap chain (\S+) \+ frame generation (\S+)') { return "SDK 2.4: swap chain $($Matches[1]) + FG $($Matches[2])" }
    return "SDK 2.4: providers not logged yet"
}

if ($Csv) {
    # Elevated child: wait, beep, record, beep. Exit code tells the parent whether it worked.
    if (-not $isAdmin) { exit 3 }
    Start-Sleep -Seconds $Delay
    [console]::Beep(880, 250)
    & $PresentMon --process_name $Process --output_file $Csv --timed $Seconds --terminate_after_timed `
        --stop_existing_session --track_frame_type --no_console_stats
    $code = $LASTEXITCODE
    [console]::Beep(660, 150); Start-Sleep -Milliseconds 120; [console]::Beep(660, 150)
    exit $code
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$target = Join-Path $OutDir ("{0:yyyyMMdd-HHmmss}-{1}.csv" -f (Get-Date), $Label)

if (-not (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Process)) -ErrorAction SilentlyContinue)) {
    Write-Warning "$Process is not running; PresentMon will record nothing until it is."
}

Write-Host "Recording '$Label': go back into the game. Beep in $Delay s = recording $Seconds s; two beeps = done." -ForegroundColor Cyan

$childArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
    '-Label', $Label, '-Seconds', $Seconds, '-Delay', $Delay, '-PresentMon', $PresentMon,
    '-OutDir', $OutDir, '-Process', $Process, '-Csv', $target) | ForEach-Object { '"{0}"' -f $_ }

if ($isAdmin) {
    & pwsh @($childArgs | ForEach-Object { $_.Trim('"') })
    $code = $LASTEXITCODE
} else {
    $child = Start-Process pwsh -Verb RunAs -WindowStyle Minimized -ArgumentList ($childArgs -join ' ') -Wait -PassThru
    $code = $child.ExitCode
}
if ($code -ne 0 -or -not (Test-Path $target)) { throw "PresentMon recording failed (exit code $code, $target)" }

$config = Get-FsrConfig
Set-Content ([IO.Path]::ChangeExtension($target, '.config.txt')) $config
Write-Host "Saved $target" -ForegroundColor Green
Write-Host "Game was using: $config" -ForegroundColor Green
& (Join-Path $PSScriptRoot "presentmon-summary.ps1") -Path $OutDir
