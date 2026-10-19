# Tests

`test\run.bat` builds the tests and runs them all, about 90 seconds. Arguments go to the runner:

```
test\run.bat                 everything but the audible tests
test\run.bat --unit          the unit tests only, a few seconds
test\run.bat mkv seek        the tests whose names contain "mkv" or "seek"
test\run.bat --audible       also the tests that record the player's sound (they play tones out loud)
test\run.bat --repeat 5      the selection 5 times over, to catch flaky tests
```

The tests use the player's own sources, built with AddressSanitizer, apart from the player's build
(`build\tests`). Every test runs in a child process, so a crash, a memory error or a hang fails that test and the
run goes on. In each child `HKEY_CURRENT_USER` is a scratch key, so no test touches your volume or saved positions;
the runner checks they are unchanged afterwards.

| Area | File | What it checks |
| --- | --- | --- |
| Subtitles | `test_subtitles.cpp` | SRT parsing, encodings, tags and styles, limits; cue lookup against a brute-force reference; fuzzing |
| Matroska | `test_mkv.cpp` | Subtitle tracks and cues across every file layout (written byte by byte by `mkv_writer.h`), seeks and their answers, key frames, codec delays; fuzzing with hang detection |
| Settings | `test_settings.cpp` | Volume and saved positions, including tampered registry values |
| Player | `test_player.cpp` | The engine: opening, sizes, clock, rates, exact and key frame seeks, frame times, volume, tracks, seek bar previews read back pixel by pixel, bad files, device paths, shutting down |
| Window | `test_window.cpp` | The window driven by keys, mouse, wheel, dropped files and media keys; subtitles, panel, resume, fullscreen, error messages; and what the screen shows: the exact frame after each seek and step, subtitles on exactly their frames |
| Overlay | `test_overlay.cpp` | Subtitle text drawn onto frames, read back |
| Sound | `test_audio.cpp` | Recorded from the output (`--audible`): the track, pitch at every speed, volume and mute, sound in step with the picture, 5.1 |

The window and screen tests open player windows on top for a moment; keep the mouse off them. A screen test skips
if the screen can't be captured (locked, remote desktop) or another window covers the player.

`tools\bench\gate.ps1` is the release check from the outside: it runs the bench clips and fails if frames drop,
subtitles show on the wrong frames, sound drifts from the picture or seeks don't land.

## Known bugs

A test marked `KNOWN_BUG("...")` shows a bug not fixed yet. It reports `XFAIL` and doesn't fail the run. Once
the bug is fixed the test passes, which reports `XPASS` and fails the run until the mark is taken off.

## Test media

`media\` is made by `make_media.ps1` with ffmpeg; `run.bat` makes it if it is missing. Each frame carries its
number as 16 bits across the picture, and each audio track is a tone of its own pitch, so a test that reads a
picture or records the sound knows exactly what it got.
