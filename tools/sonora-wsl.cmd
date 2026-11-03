@echo off
rem Starts Sonora (engine + UI) in WSL2 Debian. Double-click, or let
rem `tools\dev-wsl.ps1 shortcut` put an icon for this file on the Desktop.
rem Arguments are forwarded (stop, status, ...); the default is run.
rem The first run needs `tools\dev-wsl.ps1` once with no arguments (deps, build).
rem %* goes through as typed: copied into a variable it breaks on quotes.
rem Labels, not a parenthesised if/else: a ")" in an argument would close
rem the block and run the rest of the line as a command.
if not "%~1"=="" goto forward
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev-wsl.ps1" run
goto done
:forward
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev-wsl.ps1" %*
:done
set rc=%errorlevel%
if "%rc%"=="0" exit /b 0
echo.
echo Sonora did not start. Read the message above, then press a key.
pause >nul
exit /b %rc%
