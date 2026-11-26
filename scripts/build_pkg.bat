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
if errorlevel 1 exit /b %errorlevel%
REM publish newest setup.exe to typing\04tools (old packages removed).
REM local-only helper (scripts/ is gitignored); skipped when absent.
if not exist "%~dp0copy_setup_to_tools.ps1" (
  echo [04tools] copy_setup_to_tools.ps1 not found - skipped
  exit /b 0
)
for %%I in (pwsh.exe) do set "PS=%%~$PATH:I"
if not defined PS if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "PS=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not defined PS set "PS=powershell.exe"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0copy_setup_to_tools.ps1"
exit /b %errorlevel%
