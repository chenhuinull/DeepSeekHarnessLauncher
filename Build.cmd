@echo off
setlocal
pushd "%~dp0" || exit /b 1

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Source\build.ps1"
set "build_exit=%ERRORLEVEL%"

if "%build_exit%"=="0" (
    echo.
    echo Built: "%~dp0Out\DeepSeekHarnessLauncher.exe"
) else (
    echo.
    echo Build failed. Exit code: %build_exit%
)

if /I not "%~1"=="--no-pause" pause
popd
exit /b %build_exit%
