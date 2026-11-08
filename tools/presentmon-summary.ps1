<#
  Summarises PresentMon captures (CSV) for the FG-GPU1 frame-pacing A/B: one row per capture.

  Usage:
      pwsh -File D:\projects\mgpu-scaling\tools\presentmon-summary.ps1 [-Path <csv or folder>,...]

  Reading it. With frame generation every displayed frame should stay on screen about equally long
  (7.8 ms at 128 fps). Smoother = lower 'sd ms' and 'jitter ms', and a higher '1% low'.
    on-screen ms  mean time each frame was displayed (DisplayedTime)
    sd ms         its standard deviation
    p99 ms        99th percentile; '1% low' = 1000 / p99
    jitter ms     mean |difference| between consecutive frames' on-screen times. A "double step"
                  (generated frames shown much shorter than real ones) makes this large even when
                  the average fps looks fine.
    by type       on-screen ms per frame type, when the swap chain reports it (FSR swap chain
                  4.0.0 does; PresentMon names its generated frames "Unknown"). Equal = even pacing.
    mode          presentation mode. Driver-assisted pacing needs the game to own the display:
                  "Hardware: Independent Flip" or "Hardware Composed: Independent Flip" (an
                  overlay plane), not "Composed: Flip" (DWM composes the frame).
#>
param([string[]]$Path = @("D:\fgb\presentmon\captures"))
$ErrorActionPreference = "Stop"

$inv = [Globalization.CultureInfo]::InvariantCulture
function Num([string]$s) {
    $d = 0.0
    if ([double]::TryParse($s, [Globalization.NumberStyles]::Float, $inv, [ref]$d)) { return $d }
    return [double]::NaN
}
function Pct([double[]]$sorted, [double]$p) {
    if ($sorted.Count -eq 0) { return [double]::NaN }
    $i = [math]::Ceiling($p / 100.0 * $sorted.Count) - 1
    return $sorted[[math]::Min($sorted.Count - 1, [math]::Max(0, $i))]
}
function Shares($rows, [string]$column) {
    $groups = $rows | Group-Object $column | Sort-Object Count -Descending
    ($groups | ForEach-Object { "{0} {1:0}%" -f $_.Name, (100.0 * $_.Count / $rows.Count) }) -join ', '
}

$files = foreach ($p in $Path) {
    if (Test-Path $p -PathType Container) { Get-ChildItem $p -Filter *.csv | Sort-Object Name }
    elseif (Test-Path $p) { Get-Item $p }
    else { Write-Warning "not found: $p" }
}
if (-not $files) { Write-Warning "no captures found"; return }

$results = foreach ($file in $files) {
    $all = @(Import-Csv $file.FullName)
    if ($all.Count -lt 10) { Write-Warning "$($file.Name): only $($all.Count) frames, skipped"; continue }

    # The FFX swap chain is the one presenting the most frames (real + generated).
    $rows = @(($all | Group-Object SwapChainAddress | Sort-Object Count -Descending | Select-Object -First 1).Group)
    $columns = $rows[0].PSObject.Properties.Name

    # Column names differ between PresentMon versions (2.6 writes TimeInMs and no DisplayedTime,
    # although its README lists CPUStartTime/DisplayedTime).
    $timeColumn = @('TimeInMs', 'CPUStartTimeInMs', 'CPUStartTime') | Where-Object { $columns -contains $_ } | Select-Object -First 1
    if (-not $timeColumn) { Write-Warning "$($file.Name): no time column, skipped"; continue }
    $seconds = ((Num $rows[-1].$timeColumn) - (Num $rows[0].$timeColumn)) / 1000.0

    # On-screen time per frame: DisplayedTime when written; otherwise from the next displayed frame's
    # MsBetweenDisplayChange ("how long the previous frame was displayed"). 'NA' = never displayed.
    $shownTime = @{}
    if ($columns -contains 'DisplayedTime') {
        for ($i = 0; $i -lt $rows.Count; $i++) {
            $d = Num $rows[$i].DisplayedTime
            if ($d -gt 0) { $shownTime[$i] = $d }
        }
    } elseif ($columns -contains 'MsBetweenDisplayChange') {
        $hasUntil = $columns -contains 'MsUntilDisplayed'
        $prevShown = -1
        for ($i = 0; $i -lt $rows.Count; $i++) {
            $change = Num $rows[$i].MsBetweenDisplayChange
            if ([double]::IsNaN($change) -or ($hasUntil -and [double]::IsNaN((Num $rows[$i].MsUntilDisplayed)))) { continue }
            if ($prevShown -ge 0 -and $change -gt 0) { $shownTime[$prevShown] = $change }
            $prevShown = $i
        }
    }
    if ($shownTime.Count -lt 10 -or -not ($seconds -gt 0)) { Write-Warning "$($file.Name): no display timing, skipped"; continue }

    $order = [int[]]($shownTime.Keys | Sort-Object)
    $onScreen = [Collections.Generic.List[double]]::new()
    $jitter = [Collections.Generic.List[double]]::new()
    foreach ($i in $order) {
        if ($onScreen.Count) { $jitter.Add([math]::Abs($shownTime[$i] - $onScreen[$onScreen.Count - 1])) }
        $onScreen.Add($shownTime[$i])
    }
    $displayedFrames = $onScreen.Count + 1   # the last displayed frame has no measured on-screen time
    $sorted = [double[]]($onScreen | Sort-Object)
    $mean = ($onScreen | Measure-Object -Average).Average
    $sd = [math]::Sqrt((($onScreen | ForEach-Object { ($_ - $mean) * ($_ - $mean) }) | Measure-Object -Average).Average)
    $p99 = Pct $sorted 99

    $byType = ''
    if ($columns -contains 'FrameType') {
        $types = $order | Group-Object { $rows[$_].FrameType }
        $byType = ($types | Sort-Object Name | ForEach-Object {
                $avg = ($_.Group | ForEach-Object { $shownTime[$_] } | Measure-Object -Average).Average
                "{0} {1:0.00} ({2})" -f $_.Name, $avg, $_.Count
            }) -join ' | '
    }

    $label = $file.BaseName -replace '^\d{8}-\d{6}-', ''
    # Written by presentmon-capture.ps1 from the game's log: what the game really used.
    $configFile = [IO.Path]::ChangeExtension($file.FullName, '.config.txt')
    $config = if (Test-Path $configFile) { (Get-Content $configFile -Raw).Trim() } else { 'unknown' }
    $short = if ($config -match '^SDK 1\.1\.4') { '1.1.4' }
             elseif ($config -match '^SDK 2\.4: swap chain (\S+)') { "2.4/sc $($Matches[1])" }
             elseif ($config -match 'swap chain (\d[\d.]*)') { "2.4/sc $($Matches[1])?" }
             else { '?' }
    [pscustomobject]@{
        capture        = $label
        config         = $short
        'config text'  = $config
        's'            = [int][math]::Round($seconds, 0)
        'presented fps'= [math]::Round($rows.Count / $seconds, 1)
        'shown fps'    = [math]::Round($displayedFrames / $seconds, 1)
        'dropped %'    = [math]::Round(100.0 * (1 - $displayedFrames / $rows.Count), 1)
        'on-screen ms' = [math]::Round($mean, 2)
        'sd ms'        = [math]::Round($sd, 2)
        'p99 ms'       = [math]::Round($p99, 2)
        '1% low'       = [int][math]::Round(1000.0 / $p99, 0)
        'jitter ms'    = [math]::Round(($jitter | Measure-Object -Average).Average, 2)
        'jitter p95'   = [math]::Round((Pct ([double[]]($jitter | Sort-Object)) 95), 2)
        file           = $file.Name
        'by type'      = $byType
        mode           = Shares $rows 'PresentMode'
        tearing        = Shares $rows 'AllowsTearing'
        vsync          = Shares $rows 'SyncInterval'
    }
}

$results | Format-Table capture, config, 's', 'presented fps', 'shown fps', 'dropped %', 'on-screen ms', 'sd ms', 'p99 ms', '1% low', 'jitter ms', 'jitter p95' -AutoSize
foreach ($r in $results) {
    Write-Host ("{0}  [{1}]" -f $r.capture, $r.file) -ForegroundColor Cyan
    Write-Host ("  game used: {0}" -f $r.'config text')
    Write-Host ("  mode: {0} | tearing: {1} | sync interval: {2}" -f $r.mode, $r.tearing, $r.vsync)
    if ($r.'by type') { Write-Host ("  on-screen ms by frame type: {0}" -f $r.'by type') }
}
