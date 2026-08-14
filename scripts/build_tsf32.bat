@echo off
REM build_tsf32.bat [rebuild] - WeaselTSF Win32 (32-bit) Release build
REM Output: weasel\output\weasel.dll (Win32 TargetName=weasel, per official naming)
REM Prereq: 32-bit boost libs (scripts\build_boost32.bat, -x32- suffix)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if "%1"=="rebuild" (
  msbuild "%~dp0..\weasel\weasel.sln" /p:Configuration=Release /p:Platform=Win32 /t:WeaselTSF:Rebuild /v:minimal /nologo
) else (
  msbuild "%~dp0..\weasel\weasel.sln" /p:Configuration=Release /p:Platform=Win32 /t:WeaselTSF /v:minimal /nologo
)
exit /b %errorlevel%
