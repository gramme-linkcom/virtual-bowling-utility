@echo off
setlocal
rem Collect only what is needed to run the tool on another PC into dist\vb2026
set "ROOT=%~dp0"
set "OUT=%ROOT%dist\vb2026"
if not exist "%ROOT%build\vb2026_utility_tools.exe" (
    echo [error] exe not found. Run build.bat first.
    pause
    exit /b 1
)
mkdir "%OUT%\build" "%OUT%\models" 2>nul
copy /y "%ROOT%build\vb2026_utility_tools.exe" "%OUT%\build\" >nul
if exist "%ROOT%build\onnxruntime.dll" copy /y "%ROOT%build\onnxruntime.dll" "%OUT%\build\" >nul
copy /y "%ROOT%models\best.onnx" "%OUT%\models\" >nul
copy /y "%ROOT%run.bat" "%OUT%\" >nul
robocopy "%ROOT%toolchain-ucrt64\ucrt64\bin" "%OUT%\toolchain-ucrt64\ucrt64\bin" *.dll g++.exe /NFL /NDL /NJH /NJS
echo [ok] %OUT%
pause
