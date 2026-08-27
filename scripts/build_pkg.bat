@echo off
REM build_pkg.bat - compile installer\setup.iss -> dist\weasel-llm-setup-<ver>.exe
REM Requires Inno Setup (user-scope install location checked, then machine-wide).
setlocal
set ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe
if not exist "%ISCC%" set ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" set ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" set ISCC=C:\Program Files (x86)\Inno Setup 7\ISCC.exe
if not exist "%ISCC%" (
  echo ISCC.exe not found - install Inno Setup first
  exit /b 1
)
cd /d "%~dp0..\installer"
"%ISCC%" setup.iss
exit /b %errorlevel%
