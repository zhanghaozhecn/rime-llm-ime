@echo off
rem build_probe_wps_ctx.bat - build the WPS context-acquisition probe (standalone, no Weasel deps)
rem usage: build_probe_wps_ctx.bat [x86]   (default x64; x86 for cross-bitness ROT check)
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"
if "%1"=="x86" (call %VCVARS%\vcvarsamd64_x86.bat >nul 2>&1) else (call %VCVARS%\vcvars64.bat >nul 2>&1)
cd /d %~dp0
if "%1"=="x86" (
  cl /O2 /utf-8 probe_wps_ctx.cpp /Fe:probe_wps_ctx_x86.exe
) else (
  cl /O2 /utf-8 probe_wps_ctx.cpp /Fe:probe_wps_ctx.exe
)
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK
