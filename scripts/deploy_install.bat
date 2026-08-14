@echo off
REM deploy_install.bat - deploy latest server components to installer dir
REM (Windows auto-starts WeaselServer from installer dir on boot)
REM Double-click to run (auto-elevates).
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -Command "Start-Process cmd -ArgumentList '/c \"%~f0\"' -Verb RunAs"
  exit /b
)
taskkill /f /im WeaselServer.exe >nul 2>&1
timeout /t 2 /nobreak >nul
copy /y "%~dp0..\bin\WeaselServer.exe" "C:\Program Files\Rime\weasel-0.17.4\WeaselServer.exe" >nul
copy /y "%~dp0..\bin\rime.dll" "C:\Program Files\Rime\weasel-0.17.4\rime.dll" >nul
if errorlevel 1 (
  echo Copy failed.
  exit /b 1
)
echo Installer dir updated with latest server components.
start "" "C:\Program Files\Rime\weasel-0.17.4\WeaselServer.exe"
timeout /t 3 /nobreak >nul
