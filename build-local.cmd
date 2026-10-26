@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=D:\llvm22\bin;%PATH%"
"%~dp0..\recomp-python\Scripts\python.exe" "%~dp0scripts\native_pipeline.py" build
exit /b %errorlevel%
