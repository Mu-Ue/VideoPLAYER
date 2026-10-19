# The bench as a pass/fail check: runs the clips and fails if any measure is worse than the player has done so far.
# The tests (test\run.bat) check what the player does; this checks how well, from the outside, as the user sees and
# hears it: no dropped frames, subtitles on exactly their frames, sound in step, seeks landing, no controls popping
# up. Make the clips with make_clips.ps1 and the bench with build.bat first. It takes the screen and plays sound for
# about 5 minutes; keep the mouse still. Saved positions and the volume are put back after.
#
#   .\gate.ps1 [-Player path] [-Only clip-name-part]

param([string]$Player, [string]$Only)
$ErrorActionPreference = 'Stop'
if (-not $Player) { $Player = Join-Path $PSScriptRoot '..\..\build\VideoPLAYER.exe' }
$bench = Join-Path $PSScriptRoot 'out\bench.exe'
$clips = Join-Path $PSScriptRoot 'clips'
foreach ($need in @($Player, $bench, $clips)) { if (-not (Test-Path $need)) { throw "missing $need" } }

# Clip, seconds, seeks, extra arguments: as run.ps1.
$suite = @(
    @('r23976_1080_h264.mp4', 30, 0, @('--subs')),
    @('r23976_1080_h264_nob.mp4', 30, 0, @('--subs')),
    @('r24_1080_h264.mp4', 20, 0, @()),
    @('r30_1080_h264.mp4', 20, 0, @()),
    @('r60_1080_h264.mp4', 20, 0, @()),
    @('r23976_1080_h264_subs.mkv', 30, 6, @('--subs')),
    @('r23976_1080_h264_pcm.mkv', 30, 0, @()),
    @('v60_1080_h264.mp4', 20, 0, @()),
    @('r23976_2160_hevc_subs.mkv', 30, 6, @('--subs')),
    @('r23976_1080_h264_noindex.mkv', 20, 3, @('--subs')),
    @('r23976_2160_hevc_subs.mkv', 30, 6, @('--subs', '--cores', '2'))
)
# The limits, a little looser than the worst of recent runs so noise doesn't fail it.
$maxAvMs = 20           # |mean A/V offset|
$maxCadenceMs = 3       # mean |frame time against the timeline|
$maxInvalid = 10        # compositions whose frame number didn't read: tearing or blending
$maxSeekMs = 1500       # slowest seek; a file without an index takes about 1 s

$key = 'HKCU:\Software\VideoPLAYER'
$saved = if (Test-Path $key) { Get-ItemProperty $key } else { $null }
$csv = Join-Path ([IO.Path]::GetTempPath()) "VideoPLAYER-gate-$PID.csv"
$failures = New-Object System.Collections.Generic.List[string]
try {
    foreach ($t in $suite) {
        if ($Only -and $t[0] -notlike "*$Only*") { continue }
        Remove-ItemProperty $key -Name Positions -ErrorAction SilentlyContinue  # each from the start
        $a = @($Player, (Join-Path $clips $t[0]), '--seconds', $t[1], '--seeks', $t[2], '--label', 'gate', '--csv', $csv) + $t[3]
        & $bench @a | Out-Null
        if ($LASTEXITCODE) { $failures.Add("$($t[0]): the bench failed"); continue }
        $r = (Import-Csv $csv)[-1]
        $name = "$($t[0])$(if ($t[3] -contains '--cores') { ' (2 cores)' })"
        $n = @{}
        foreach ($p in $r.PSObject.Properties) {
            $v = 0.0
            $n[$p.Name] = if ([double]::TryParse($p.Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$v)) { $v } else { [double]::NaN }
        }
        $checks = New-Object System.Collections.Generic.List[object]
        $check = { param($what, [bool]$ok, $shown) $checks.Add(@($what, $ok, $shown)) }
        & $check 'dropped frames' ($n.dropped -eq 0) $r.dropped
        & $check 'frame cadence' ($n.cadence_mean_abs_ms -le $maxCadenceMs) "$($r.cadence_mean_abs_ms) ms"
        & $check 'unreadable frames' ($n.invalid_samples -le $maxInvalid) $r.invalid_samples
        & $check 'controls popping up on seeks' ($n.seek_popups -eq 0) $r.seek_popups
        & $check 'seeks landed' ($n.seeks -eq $t[2]) "$($r.seeks) of $($t[2])"
        if ($t[3] -contains '--subs') {
            & $check 'subtitles on the wrong frames' (($n.sub_wrong -eq 0) -and ($n.sub_checked -gt 100)) "$($r.sub_wrong) of $($r.sub_checked)"
        }
        if ($n.av_n -gt 0) {
            & $check 'A/V offset' ([math]::Abs($n.av_mean_ms) -le $maxAvMs) "$($r.av_mean_ms) ms"
        } else {
            & $check 'A/V offset' $false 'no clicks heard'
        }
        if ($t[2] -gt 0) { & $check 'seek time' ($n.seek_max_ms -le $maxSeekMs) "$($r.seek_max_ms) ms" }
        $bad = $checks.Where({ -not $_[1] })
        if ($bad) {
            foreach ($b in $bad) { $failures.Add("${name}: $($b[0]): $($b[2])") }
            Write-Host "[FAIL ] $name" -ForegroundColor Red
            foreach ($b in $bad) { Write-Host "  $($b[0]): $($b[2])" }
        } else {
            Write-Host "[PASS ] $name  (A/V $($r.av_mean_ms) ms, cadence $($r.cadence_mean_abs_ms) ms)"
        }
        Start-Sleep -Seconds 1
    }
} finally {
    # The user's saved positions and volume, as they were.
    if ($saved) {
        foreach ($v in 'Positions', 'Volume', 'Muted') {
            if ($null -ne $saved.$v) {
                $type = if ($v -eq 'Positions') { 'Binary' } else { 'DWord' }
                New-ItemProperty $key -Name $v -Value $saved.$v -PropertyType $type -Force | Out-Null
            } else {
                Remove-ItemProperty $key -Name $v -ErrorAction SilentlyContinue
            }
        }
    }
    Remove-Item $csv -ErrorAction SilentlyContinue
}
if ($failures.Count) {
    Write-Host "`n$($failures.Count) failed:"
    $failures | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "`nAll passed."
