<#
PhoneMirror lab: runs every scenario in lab\scenarios against build\Release\PhoneMirror.exe with a virtual
iPhone (no phone, no network) and checks the results.

  .\lab\run-lab.ps1                 # all scenarios
  .\lab\run-lab.ps1 -Only pointer   # scenarios whose name contains "pointer"
  .\lab\run-lab.ps1 -Watch          # slower, so you can watch what happens

The lab drives the app with real mouse/keyboard input: don't touch the mouse or keyboard while it runs
(about a minute). It never types into other windows - input steps are skipped if the lab window is not in front.

Scenario assertions (lines in the .lab file):
  expect <regex>              the log must contain a matching line
  expect-not <regex>          the log must not contain one
  expect-files <glob> <min>   at least <min> files under the run's output folder
  expect-probe <glob> <regex> ffprobe of every matching file must match (needs ffprobe on PATH or winget FFmpeg)
#>
param([string]$Only = '', [switch]$Watch)

$ErrorActionPreference = 'Stop'
$lab = $PSScriptRoot
$root = Split-Path -Parent $lab
$exe = Join-Path $root 'build\Release\PhoneMirror.exe'
if (-not (Test-Path $exe)) { throw "Build first: $exe not found" }

function Find-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $winget = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($winget) { return $winget.FullName }
    return $null
}
$ffmpeg = Find-Tool 'ffmpeg'
$ffprobe = Find-Tool 'ffprobe'

# ---- fixtures (generated once) -------------------------------------------------------------------------
$fixtures = Join-Path $lab 'fixtures'
New-Item -ItemType Directory -Force $fixtures | Out-Null
$clips = @{
    'portrait.h265'  = @('-f', 'lavfi', '-i', 'testsrc2=size=994x2160:rate=60', '-t', '8', '-c:v', 'libx265', '-preset', 'ultrafast',
                         '-x265-params', 'bframes=0:keyint=60:log-level=error', '-pix_fmt', 'yuv420p')
    'landscape.h265' = @('-f', 'lavfi', '-i', 'testsrc2=size=2160x994:rate=60', '-t', '8', '-c:v', 'libx265', '-preset', 'ultrafast',
                         '-x265-params', 'bframes=0:keyint=60:log-level=error', '-pix_fmt', 'yuv420p')
    'portrait.h264'  = @('-f', 'lavfi', '-i', 'testsrc2=size=496x1080:rate=60', '-t', '8', '-c:v', 'libx264', '-preset', 'ultrafast',
                         '-bf', '0', '-g', '60', '-pix_fmt', 'yuv420p')
    'tone.m4a'       = @('-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=30', '-af', 'volume=0.03',
                         '-ac', '2', '-c:a', 'aac', '-b:a', '96k')
}
foreach ($name in $clips.Keys) {
    $path = Join-Path $fixtures $name
    if (Test-Path $path) { continue }
    if (-not $ffmpeg) { throw "ffmpeg is needed once to generate lab fixtures (winget install Gyan.FFmpeg)" }
    Write-Host "Generating fixture $name"
    & $ffmpeg -hide_banner -loglevel error -y @($clips[$name]) $path
}

# ---- scenarios -----------------------------------------------------------------------------------------
$scenarios = Get-ChildItem (Join-Path $lab 'scenarios') -Filter '*.lab' | Where-Object { $_.BaseName -like "*$Only*" } |
    Sort-Object Name
$results = @()
foreach ($scenario in $scenarios) {
    $out = Join-Path $lab "out\$($scenario.BaseName)"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force $out | Out-Null
    $lines = Get-Content $scenario.FullName
    $last = ($lines | Where-Object { $_ -match '^\s*(\d+)\s' } | ForEach-Object { [int]($Matches[1]) } | Measure-Object -Maximum).Maximum
    $timeout = [int]($last / 1000) + 25

    Write-Host -NoNewline ("{0,-28}" -f $scenario.BaseName)
    $p = Start-Process $exe -ArgumentList '--lab', "`"$($scenario.FullName)`"", '--data-dir', "`"$out`"" -PassThru
    $finished = $p.WaitForExit($timeout * 1000)
    if (-not $finished) { Stop-Process -Id $p.Id -Force }
    $logPath = Join-Path $out 'phonemirror.log'
    $log = if (Test-Path $logPath) { Get-Content $logPath } else { @() }

    $failures = @()
    if (-not $finished) { $failures += "timed out after $timeout s" }
    foreach ($line in $lines) {
        if ($line -match '^\s*expect\s+(.+)$') {
            if (-not ($log | Select-String -Pattern $Matches[1] -Quiet)) { $failures += "missing: $($Matches[1])" }
        } elseif ($line -match '^\s*expect-not\s+(.+)$') {
            $hit = $log | Select-String -Pattern $Matches[1] | Select-Object -First 1
            if ($hit) { $failures += "unexpected: $($hit.Line.Trim())" }
        } elseif ($line -match '^\s*expect-files\s+(\S+)\s+(\d+)$') {
            $count = @(Get-ChildItem (Join-Path $out $Matches[1]) -ErrorAction SilentlyContinue).Count
            if ($count -lt [int]$Matches[2]) { $failures += "expected >= $($Matches[2]) files $($Matches[1]), found $count" }
        } elseif ($line -match '^\s*expect-probe\s+(\S+)\s+(.+)$') {
            $pattern = $Matches[2]
            if (-not $ffprobe) { $failures += 'ffprobe not found'; continue }
            foreach ($f in Get-ChildItem (Join-Path $out $Matches[1]) -ErrorAction SilentlyContinue) {
                $info = & $ffprobe -v error -show_entries 'stream=codec_name,width,height,sample_rate:format=duration' -of compact $f.FullName | Out-String
                if ($info -notmatch $pattern) { $failures += "probe $($f.Name) !~ $pattern :: $($info.Trim() -replace '\s+', ' ')" }
            }
        }
    }
    if ($log | Select-String 'Crashed' -Quiet) { $failures += 'crash dump written' }
    if ($failures.Count) {
        Write-Host 'FAIL' -ForegroundColor Red
        $failures | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkRed }
    } else {
        Write-Host 'PASS' -ForegroundColor Green
    }
    $results += [pscustomobject]@{ Scenario = $scenario.BaseName; Passed = ($failures.Count -eq 0); Log = $logPath }
    if ($Watch) { Start-Sleep 2 }
}

$passed = @($results | Where-Object Passed).Count
Write-Host ''
Write-Host "$passed / $($results.Count) scenarios passed. Logs and captures: $(Join-Path $lab 'out')"
exit ([int]($passed -ne $results.Count))
