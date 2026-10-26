@echo off
setlocal
python "%~dp0scripts\analyze_local.py" %*
exit /b %errorlevel%
