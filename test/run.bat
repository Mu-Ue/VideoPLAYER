@echo off
rem Builds and runs the tests; arguments go to the runner (tests --help). Makes the test media first if it is
rem missing, which needs ffmpeg.
setlocal
set "HERE=%~dp0"
if not exist "%HERE%media\clip30.mp4" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%make_media.ps1" || exit /b 1
)
call "%HERE%build.bat" || exit /b 1
"%HERE%..\build\tests\tests.exe" %*
