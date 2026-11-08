<#
  Keeps every Fallout 4 run's logs for A/B tests. MGPUScaling.log and ReShade.log are overwritten on
  each game start, so back-to-back runs would otherwise lose all but the last.

  Waits for Fallout4.exe, notes bDirectGPUCopy from the MCM settings when it starts, and when it exits
  copies MGPUScaling.log, ReShade.log and MCM\Settings\MGPUScaling.ini into
  <Out>\<start time>-direct<0|1>-<hdr|sdr>\ (hdr = the log shows an HDR10/scRGB back buffer, i.e.
  ReShade's AutoHDR was active). Prints one line per start and exit.

      pwsh -NoProfile -ExecutionPolicy Bypass -File .\save-runs.ps1             # 3 hours, D:\fgb\runs
      pwsh -NoProfile -ExecutionPolicy Bypass -File .\save-runs.ps1 -Hours 1 -Out D:\fgb\runs-ram

  Compare runs with the 10 s lines: "game fps", "render GPU: idle between frames ... frame from its
  first command to its end", "timeline ms after Present", "direct GPU copy: N/N frames".
#>
param(
    [string]$Out = 'D:\fgb\runs',
    [double]$Hours = 3,
    [string]$GameDir = 'C:\SteamLibrary\steamapps\common\Fallout 4'
)

$log = Join-Path $env:USERPROFILE 'Documents\My Games\Fallout4\F4SE\MGPUScaling.log'
$ini = Join-Path $GameDir 'Data\MCM\Settings\MGPUScaling.ini'
New-Item -ItemType Directory -Force $Out | Out-Null
$deadline = (Get-Date).AddHours($Hours)
Write-Host "watching for Fallout4.exe until $($deadline.ToString('HH:mm')); logs go to $Out"

while ((Get-Date) -lt $deadline) {
    $process = Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $process) { Start-Sleep -Seconds 2; continue }

    $start = Get-Date
    $direct = $null
    if (Test-Path $ini) {
        $match = Select-String -Path $ini -Pattern '^\s*bDirectGPUCopy\s*=\s*(\d)' | Select-Object -Last 1
        if ($match) { $direct = $match.Matches[0].Groups[1].Value }
    }
    if (-not $direct) { $direct = 'default' }
    Write-Host "$($start.ToString('HH:mm:ss')) game started (pid $($process.Id)), bDirectGPUCopy=$direct"
    $process.WaitForExit()
    Start-Sleep -Seconds 1

    $hdr = if (Select-String -Path $log -Pattern 'but is format (24|10)' -Quiet) { 'hdr' } else { 'sdr' }
    $dir = Join-Path $Out ("{0}-direct{1}-{2}" -f $start.ToString('yyyyMMdd-HHmmss'), $direct, $hdr)
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item $log, (Join-Path $GameDir 'ReShade.log'), $ini $dir -ErrorAction SilentlyContinue
    $windows = (Select-String -Path $log -Pattern '\] \[FG-GPU1\] [0-9.]+ game fps').Count
    Write-Host "$((Get-Date).ToString('HH:mm:ss')) game exited; $windows ten-second windows saved to $dir"
}
Write-Host 'watcher stopped (time limit)'
