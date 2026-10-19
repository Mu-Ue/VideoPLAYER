# Makes the test media in test\media with ffmpeg. Small and short, so all of it is a few hundred KB.
#
# Each frame carries its own number as 16 bits, a row of black or white blocks with the same row inverted below
# (as tools\bench\make_clips.ps1 draws them), so a test that reads a picture back knows which frame it is. Every
# 2 seconds a white box flashes in the middle for one frame, with a 10 ms click on clips meant for A/V sync.
# Audio tracks are steady tones of their own pitch, so a test that records the sound knows which track plays.

param([string]$Only)
$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot 'media'
New-Item -ItemType Directory -Force $out | Out-Null

function Video([string]$rate, [double]$seconds, [int]$w, [int]$h) {
    $f = [double](Invoke-Expression ($rate -replace '/', ' / '))
    $flash = [math]::Round(2 * $f)
    $geq = "lum='if(between(Y,H*0.15,H*0.2-1),if(mod(floor(N/pow(2,floor(X*16/W))),2),255,0),if(between(Y,H*0.2,H*0.25-1),if(mod(floor(N/pow(2,floor(X*16/W))),2),0,255),if(eq(mod(N,$flash),0)*between(Y,H*0.35,H*0.65)*between(X,W*0.35,W*0.65),255,96)))':cb=128:cr=128"
    "color=c=gray:s=256x144:r=${rate}:d=$seconds,format=yuv444p,geq=$geq,scale=${w}:${h}:flags=neighbor,format=yuv420p"
}

function Tone([double]$hz, [double]$seconds, [int]$channels = 2) {
    $layout = @{ 1 = 'mono'; 2 = 'stereo'; 6 = '5.1' }[$channels]
    "sine=frequency=${hz}:sample_rate=48000:duration=$seconds,volume=0.5,aformat=channel_layouts=$layout"
}

# A click at each flash frame of a clip at this rate.
function Clicks([string]$rate, [double]$seconds) {
    $f = [double](Invoke-Expression ($rate -replace '/', ' / '))
    $flash = [math]::Round(2 * $f)
    "aevalsrc='if(lt(t-floor(t*$f/$flash)*$flash/$f,0.01),0.8*sin(2*PI*1000*t),0)':s=48000:d=$seconds"
}

function Make([string]$name, [string[]]$arguments) {
    if ($Only -and $name -ne $Only) { return }
    $file = Join-Path $out $name
    Write-Host "Making $file"
    & ffmpeg -nostdin -y -v error @arguments $file
    if ($LASTEXITCODE) { throw "ffmpeg failed for $name" }
}

function Srt([string]$name, [string[]]$cues) {
    [IO.File]::WriteAllText((Join-Path $out $name), ($cues -join "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))
}

$x264 = @('-c:v', 'libx264', '-preset', 'veryfast', '-crf', '12', '-sc_threshold', '0')

# Subtitles muxed into the MKVs; named so no clip takes them for its own sidecars.
Srt 'subs_en.srt' @('1', '00:00:00,500 --> 00:00:01,500', 'One', '', '2', '00:00:02,000 --> 00:00:03,000', '<i>Two</i>', '',
                    '3', '00:00:04,000 --> 00:00:05,500', 'Three', 'second line', '')
Srt 'subs_de.srt' @('1', '00:00:00,500 --> 00:00:01,500', 'Eins', '', '2', '00:00:02,000 --> 00:00:03,000', 'Zwei', '',
                    '3', '00:00:04,000 --> 00:00:05,500', 'Drei', '')
$en = Join-Path $out 'subs_en.srt'
$de = Join-Path $out 'subs_de.srt'

# 30 fps with B-frames, so an edit list starts it later; a key frame each second; a 440 Hz tone.
Make 'clip30.mp4' (@('-f', 'lavfi', '-i', (Video '30' 6 320 180), '-f', 'lavfi', '-i', (Tone 440 6)) + $x264 +
    @('-g', '30', '-c:a', 'aac', '-b:a', '96k', '-shortest'))
# The same without sound.
Make 'silent30.mp4' (@('-f', 'lavfi', '-i', (Video '30' 6 320 180)) + $x264 + @('-g', '30'))
# 60 fps, for the time between frames.
Make 'clip60.mp4' (@('-f', 'lavfi', '-i', (Video '60' 3 320 180)) + $x264 + @('-g', '60'))
# Key frames 5 s apart, so fast seeks visibly land on them.
Make 'gop5s.mp4' (@('-f', 'lavfi', '-i', (Video '25' 12 320 180)) + $x264 + @('-g', '125', '-keyint_min', '125'))
# The same without B-frames, so without an edit list.
Make 'gop5s_nob.mp4' (@('-f', 'lavfi', '-i', (Video '25' 12 320 180)) + $x264 + @('-g', '125', '-keyint_min', '125', '-bf', '0'))
# Key frames 20 s apart, at 0 and 20 s, so the nearest to a 5 s step can be behind where it started.
Make 'gop20s.mp4' (@('-f', 'lavfi', '-i', (Video '25' 24 320 180)) + $x264 + @('-g', '500', '-keyint_min', '500', '-bf', '0'))
# Clicks on the flash frames, AAC with its encoder delay (and an edit list skipping it) and PCM without.
Make 'av_aac.mp4' (@('-f', 'lavfi', '-i', (Video '30' 8 320 180), '-f', 'lavfi', '-i', (Clicks '30' 8)) + $x264 +
    @('-g', '30', '-c:a', 'aac', '-b:a', '128k', '-shortest'))
Make 'av_pcm.mkv' (@('-f', 'lavfi', '-i', (Video '30' 8 320 180), '-f', 'lavfi', '-i', (Clicks '30' 8)) + $x264 +
    @('-g', '30', '-c:a', 'pcm_s16le', '-shortest'))
# Two audio tracks, 440 Hz in English and 880 Hz in French with a name, and SRT, SRT and ASS subtitles.
Make 'tracks.mkv' (@('-f', 'lavfi', '-i', (Video '25' 6 320 180), '-f', 'lavfi', '-i', (Tone 440 6),
        '-f', 'lavfi', '-i', (Tone 880 6), '-i', $en, '-i', $de, '-i', $en,
        '-map', '0:v', '-map', '1:a', '-map', '2:a', '-map', '3:s', '-map', '4:s', '-map', '5:s') + $x264 +
    @('-g', '25', '-c:a', 'aac', '-b:a', '96k', '-c:s:0', 'srt', '-c:s:1', 'srt', '-c:s:2', 'ass',
      '-metadata:s:a:0', 'language=eng', '-metadata:s:a:1', 'language=fre', '-metadata:s:a:1', 'title=Commentary',
      '-metadata:s:s:0', 'language=eng', '-metadata:s:s:1', 'language=ger', '-metadata:s:s:2', 'language=jpn',
      '-metadata:s:s:2', 'title=Styled', '-t', '6'))
# The same picture and English subtitles with no index, as a live stream writes it.
Make 'noindex.mkv' (@('-f', 'lavfi', '-i', (Video '25' 6 320 180), '-f', 'lavfi', '-i', (Tone 440 6), '-i', $en,
        '-map', '0:v', '-map', '1:a', '-map', '2:s') + $x264 + @('-g', '25', '-c:a', 'aac', '-c:s', 'srt', '-live', '1', '-t', '6'))
# 5.1 sound, uncompressed and AAC.
Make 'surround.mkv' (@('-f', 'lavfi', '-i', (Video '25' 4 320 180), '-f', 'lavfi', '-i', (Tone 440 4 6)) + $x264 +
    @('-g', '25', '-c:a', 'pcm_s16le', '-shortest'))
Make 'surround.mp4' (@('-f', 'lavfi', '-i', (Video '25' 4 320 180), '-f', 'lavfi', '-i', (Tone 440 4 6)) + $x264 +
    @('-g', '25', '-c:a', 'aac', '-b:a', '256k', '-shortest'))
# Sound alone.
Make 'audio.m4a' @('-f', 'lavfi', '-i', (Tone 440 4), '-c:a', 'aac', '-b:a', '96k')
# Non-square pixels: 320 x 240 stored, shown 16:9, 427 x 240.
Make 'anamorphic.mp4' (@('-f', 'lavfi', '-i', ((Video '25' 3 320 240) + ',setsar=4/3')) + $x264 + @('-g', '25'))
# Turned a quarter: stored 320 x 180, shown upright 180 x 320. Copied, as encoding would turn the frames instead.
Make 'rotated.mp4' @('-display_rotation', '90', '-i', (Join-Path $out 'silent30.mp4'), '-c', 'copy')
# A frame size that is no multiple of 16.
Make 'odd.mp4' (@('-f', 'lavfi', '-i', ((Video '25' 3 322 182) + ',setsar=1')) + $x264 + @('-g', '25'))
