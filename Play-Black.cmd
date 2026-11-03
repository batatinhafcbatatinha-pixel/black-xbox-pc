@echo off
setlocal
cd /d "%~dp0"
set "BIN=project\out\black_native.exe"
if exist "build-location.txt" set /p BIN_DIR=<"build-location.txt"
if defined BIN_DIR set "BIN=%BIN_DIR%\black_native.exe"
if not exist "%BIN%" (
  echo The game is not built yet: run Setup.cmd first.
  echo O jogo ainda nao foi montado: rode o Setup.cmd primeiro.
  pause
  exit /b 1
)
start "Black" "%BIN%" "%~dp0game_files"
