@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

rem Find the toolchain inside the project (same order as build.bat)
set "TC="
if exist "%ROOT%ucrt64\bin\g++.exe" set "TC=%ROOT%ucrt64"
if not defined TC if exist "%ROOT%toolchain-ucrt64\ucrt64\bin\g++.exe" set "TC=%ROOT%toolchain-ucrt64\ucrt64"

if not defined TC (
    echo [error] toolchain not found. See build.bat for the expected location.
    pause
    exit /b 1
)

if not exist "%ROOT%build\vb2026_utility_tools.exe" (
    echo [error] exe not found. Run build.bat first.
    pause
    exit /b 1
)

rem DLLs (OpenCV, libstdc++ ...) are found through PATH only for this window
set "PATH=%TC%\bin;%PATH%"
"%ROOT%build\vb2026_utility_tools.exe" %*
pause 