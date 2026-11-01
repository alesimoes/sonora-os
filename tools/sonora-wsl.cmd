@echo off
rem Starts Sonora (engine + UI) in WSL2 Debian. Double-click, or let
rem `tools\dev-wsl.ps1 shortcut` put an icon for this file on the Desktop.
rem Arguments are forwarded (stop, status, ...); the default is run.
rem The first run needs `tools\dev-wsl.ps1` once with no arguments (deps, build).
set "STEP=%*"
if "%STEP%"=="" set "STEP=run"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev-wsl.ps1" %STEP%
if errorlevel 1 (
    echo.
    echo Sonora did not start. Read the message above, then press a key.
    pause >nul
)
