@echo off
rem Prepares the build files from your own disc (tools, game check, function lists, translation) without compiling.
rem Setup.cmd does this and the compile; run this when you want to build by hand afterwards.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" -PrepareOnly %*
echo.
pause
