@echo off
setlocal
"%~dp0..\recomp-python\Scripts\python.exe" "%~dp0scripts\native_pipeline.py" generate
exit /b %errorlevel%
