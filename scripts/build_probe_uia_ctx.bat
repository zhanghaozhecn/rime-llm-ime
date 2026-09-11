@echo off
rem build_probe_uia_ctx.bat - build the UIA TextPattern caret-context probe (standalone)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d %~dp0
cl /O2 /utf-8 probe_uia_ctx.cpp /Fe:probe_uia_ctx.exe
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK
