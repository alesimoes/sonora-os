@echo off
rem Starts Sonora (engine + UI) in WSL2 Debian. Double-click, or let
rem `tools\dev-wsl.ps1 shortcut` put an icon for this file on the Desktop.
rem The first run needs `tools\dev-wsl.ps1` once with no arguments (deps, build).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev-wsl.ps1" run
if errorlevel 1 (
    echo.
    echo Sonora did not start. Read the message above, then press a key.
    pause >nul
)
