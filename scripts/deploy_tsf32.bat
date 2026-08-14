@echo off
REM deploy_tsf32.bat - deploy BOTH TSF DLLs via official WeaselSetup flow
REM Double-click to run (auto-elevates)
REM Steps: copy output\weasel.dll (Win32 LLM build) + output\weaselx64.dll
REM (x64 LLM build) to installer dir, then WeaselSetup /u + /i
REM (official flow: SysWOW64 + System32 deploy + registration)
REM NOTE: WeaselSetup only registers the 64-bit view; if 32-bit apps
REM output English letters, re-register manually:
REM   set TEXTSERVICE_PROFILE=hans
REM   C:\Windows\SysWOW64\regsvr32.exe /s C:\Windows\SysWOW64\weasel.dll
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -Command "Start-Process cmd -ArgumentList '/c \"%~f0\"' -Verb RunAs"
  exit /b
)
taskkill /f /im wps.exe >nul 2>&1
taskkill /f /im wpscloudsvr.exe >nul 2>&1
taskkill /f /im WeaselServer.exe >nul 2>&1
timeout /t 2 /nobreak >nul
copy /y "%~dp0..\weasel\output\weasel.dll" "C:\Program Files\Rime\weasel-0.17.4\weasel.dll" >nul
copy /y "%~dp0..\weasel\output\weaselx64.dll" "C:\Program Files\Rime\weasel-0.17.4\weaselx64.dll" >nul
echo Both LLM TSF DLLs copied to installer dir.
"C:\Program Files\Rime\weasel-0.17.4\WeaselSetup.exe" /u
"C:\Program Files\Rime\weasel-0.17.4\WeaselSetup.exe" /i
echo WeaselSetup /u /i done.
start "" "%~dp0..\bin\WeaselServer.exe"
echo WeaselServer restarted.
timeout /t 3 /nobreak >nul
