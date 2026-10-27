@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "runs" mkdir "runs"
"D:\llvm22\bin\clang-cl.exe" /nologo /Iproject/src /Iproject/src/recomp /Iproject/src/recomp/gen project/tests/texture_state_test.c project/src/native_d3d/black_texture_state.c /Feruns/texture_state_test.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
runs\texture_state_test.exe
exit /b %errorlevel%
