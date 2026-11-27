# build-watch.ps1 -- rebuild the ESP-IDF project automatically while editing.
#
# Run once, in a normal PowerShell window, and leave the window open:
#     powershell -ExecutionPolicy Bypass -File .\build-watch.ps1
#
# What it does:
#   1. watches examples\3.7einkDvb (sources, CMakeLists, sdkconfig, partition table);
#   2. rebuilds 3 s after edits stop (so one save = one build);
#   3. writes the full log to  .esp-build\last.log
#      and a one-line verdict to .esp-build\status.txt  (plus history.log).
#
# Options:
#     -NoInitialBuild         skip the build at startup
#     -Project examples\xxx   watch another example
[CmdletBinding()]
param(
    [string]$Project = 'examples\3.7einkDvb',
    [int]$DebounceSeconds = 3,
    [int]$PollMilliseconds = 1000,
    [switch]$NoInitialBuild
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$proj = Join-Path $root $Project
if (-not (Test-Path (Join-Path $proj 'CMakeLists.txt'))) { throw "Not an ESP-IDF project: $proj" }

$outDir     = Join-Path $root '.esp-build'
$statusPath = Join-Path $outDir 'status.txt'
$logPath    = Join-Path $outDir 'last.log'
$histPath   = Join-Path $outDir 'history.log'
$beatPath   = Join-Path $outDir 'heartbeat.txt'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Activate the ESP-IDF environment (native Windows install, see AGENTS.md)
$activation = Get-ChildItem 'C:\Espressif\tools\Microsoft.v*.PowerShell_profile.ps1' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $activation) { throw 'ESP-IDF activation script not found under C:\Espressif\tools' }
. $activation.FullName | Out-Null

function Get-SourceStamp {
    $files = @(Get-ChildItem (Join-Path $proj 'main') -Recurse -File -ErrorAction SilentlyContinue)
    foreach ($name in @('CMakeLists.txt', 'sdkconfig', 'sdkconfig.defaults', 'partitions.csv')) {
        $p = Join-Path $proj $name
        if (Test-Path $p) { $files += Get-Item $p }
    }
    if ($files.Count -eq 0) { return '0|0|0' }
    $max  = ($files | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum.Ticks
    $size = ($files | Measure-Object -Property Length -Sum).Sum
    return ("{0}|{1}|{2}" -f $max, $files.Count, $size)
}

function Invoke-BuildNow {
    $started = Get-Date
    Set-Content -Path $statusPath -Value ("BUILDING " + $started.ToString('o')) -Encoding ASCII
    Write-Host ("[{0}] build started ..." -f $started.ToString('HH:mm:ss')) -ForegroundColor Cyan

    # stderr of a native command becomes ErrorRecord under 5.1; relax EAP so it
    # is captured into the log instead of aborting the watcher.
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $text = & idf.py -C $proj build 2>&1 | Out-String -Width 400
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old

    $secs = ((Get-Date) - $started).TotalSeconds
    [IO.File]::WriteAllText($logPath, $text, (New-Object System.Text.UTF8Encoding($false)))

    $verdict = if ($code -eq 0) { 'OK' } else { 'FAILED' }
    $line = ("{0} exit={1} {2:N1}s {3}" -f $verdict, $code, $secs, (Get-Date).ToString('o'))
    Set-Content -Path $statusPath -Value $line -Encoding ASCII
    Add-Content -Path $histPath -Value $line -Encoding ASCII

    $color = if ($code -eq 0) { 'Green' } else { 'Red' }
    Write-Host ("[{0}] {1} in {2:N1}s" -f (Get-Date).ToString('HH:mm:ss'), $verdict, $secs) -ForegroundColor $color
}

$stamp = Get-SourceStamp
Set-Content -Path $beatPath -Value ("started " + (Get-Date).ToString('o') + " pid=$PID") -Encoding ASCII
Set-Content -Path $statusPath -Value ("IDLE " + (Get-Date).ToString('o')) -Encoding ASCII
Write-Host "Watching $proj  (poll ${PollMilliseconds} ms, debounce ${DebounceSeconds} s). Ctrl+C to stop."

if (-not $NoInitialBuild) { Invoke-BuildNow }

$tick = 0
while ($true) {
    Start-Sleep -Milliseconds $PollMilliseconds
    $tick++
    if (($tick % 10) -eq 0) {
        Set-Content -Path $beatPath -Value ((Get-Date).ToString('o')) -Encoding ASCII
    }

    $now = Get-SourceStamp
    if ($now -eq $stamp) { continue }

    # wait until edits settle, then build once
    do {
        $prev = $now
        Start-Sleep -Seconds $DebounceSeconds
        $now = Get-SourceStamp
    } while ($now -ne $prev)

    $stamp = $now
    Invoke-BuildNow
}
