@echo off
REM build_rime.bat - librime x64 Release build
REM Prereq: run scripts\cmake_rime.bat once to generate librime\build\*.sln
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild "%~dp0..\librime\build\src\rime.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
exit /b %errorlevel%
