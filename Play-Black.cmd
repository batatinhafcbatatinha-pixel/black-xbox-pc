@echo off
setlocal
cd /d "%~dp0"
set "BIN=project\out\black_native.exe"
if exist "build-location.txt" set /p BIN_DIR=<"build-location.txt"
if defined BIN_DIR set "BIN=%BIN_DIR%\black_native.exe"
if not exist "%BIN%" (
  echo Executavel nativo ainda nao compilado. Execute build-local.cmd.
  pause
  exit /b 1
)
start "Black" "%BIN%" "%~dp0game_files"
