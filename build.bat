@echo off
setlocal
set "ROOT=%~dp0"

rem プロジェクト内の toolchain を探す（上から順に見る）
set "TC="
if exist "%ROOT%ucrt64\bin\g++.exe" set "TC=%ROOT%ucrt64"
if not defined TC if exist "%ROOT%toolchain-ucrt64\ucrt64\bin\g++.exe" set "TC=%ROOT%toolchain-ucrt64\ucrt64"

if not defined TC (
    echo [error] プロジェクト内に g++ が見つかりません
    echo   次のどちらかの場所に ucrt64 フォルダーを置いてください
    echo     %ROOT%ucrt64\bin\g++.exe
    echo     %ROOT%toolchain-ucrt64\ucrt64\bin\g++.exe
    exit /b 1
)

echo [toolchain] %TC%
rem PC に入っている別の g++ (w64devkit など) を拾わないよう、使うものを明示する
set "PATH=%TC%\bin;%PATH%"

cmake -G Ninja -DCMAKE_CXX_COMPILER="%TC%/bin/g++.exe" -DCMAKE_MAKE_PROGRAM="%TC%/bin/ninja.exe" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -S "%ROOT%." -B "%ROOT%build" || exit /b 1
cmake --build "%ROOT%build" || exit /b 1

echo.
echo [ok] %ROOT%build\vb2026_utility_tools.exe
pause