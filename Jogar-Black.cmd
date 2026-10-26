@echo off
setlocal
cd /d "%~dp0"
if exist "build-release\Black_Recomp.exe" (
  start "" "build-release\Black_Recomp.exe"
) else if exist "Black_Recomp.exe" (
  start "" "Black_Recomp.exe"
) else (
  echo Executavel do port ainda nao compilado.
  echo Execute build-local.cmd apos a geracao dos fontes.
  pause
)
