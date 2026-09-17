@echo off
setlocal
pushd "%~dp0" || exit /b 1

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0native\build.ps1"
set "build_exit=%ERRORLEVEL%"

if "%build_exit%"=="0" (
    echo.
    echo Published: "%~dp0dist\DeepSeekHarnessLauncher.exe"
) else (
    echo.
    echo Build failed. Exit code: %build_exit%
)

if /I not "%~1"=="--no-pause" pause
popd
exit /b %build_exit%
