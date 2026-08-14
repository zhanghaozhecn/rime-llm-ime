@echo off
REM ============================================
REM  rime-llm-ime one-click deploy script (2026-08-14)
REM  Full build incl. 4 TSF modules + WPS lag detection:
REM    rime.dll       B9D9F983  librime + llm_filter (lag detection)
REM    WeaselServer.exe         server + SET_CONTEXT_TEXT/RESET_CONTEXT IPC
REM    weaselx64.dll  DF476ACE  64-bit TSF: 0.17.4 + LLM ctx + 4 modules
REM    weasel32.dll             32-bit TSF: same features, Win32 build
REM  Behavior: WPS=commit history rerank (AI-hist), others=TSF ctx (AI-TSF)
REM  Place this file in bin\ and double-click it on the TARGET machine.
REM  Prereq: official weasel 0.17.4 installed (provides registry/TSF
REM  registration, installer dir, tray app, data\)
REM ============================================

REM auto-elevate to admin
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator privileges, please confirm the UAC prompt...
  powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

set DEST=C:\Program Files\Rime\weasel-0.17.4
set SRC=%~dp0

if not exist "%DEST%" (
  echo [ERROR] Rime install dir not found: %DEST%
  echo Install official Rime 0.17.4 first, then re-run this script
  pause
  exit /b 1
)

echo [1/7] Checking LLM model...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy_llm_model.ps1"

echo [2/7] Stopping WeaselServer...
taskkill /f /im WeaselServer.exe >nul 2>&1
timeout /t 3 /nobreak >nul

echo [3/7] Copying LLM files to install dir...
copy /y "%SRC%rime.dll" "%DEST%\" >nul
copy /y "%SRC%weaselx64.dll" "%DEST%\" >nul
copy /y "%SRC%weasel32.dll" "%DEST%\weasel.dll" >nul
copy /y "%SRC%WeaselServer.exe" "%DEST%\" >nul
copy /y "%SRC%WeaselDeployer.exe" "%DEST%\" >nul
copy /y "%SRC%opencc.dll" "%DEST%\" >nul
copy /y "%SRC%vcomp140.dll" "%DEST%\" >nul
echo     done (rime / weaselx64 / weasel32-^>weasel / server / deployer / opencc / vcomp140)

echo [4/7] Official WeaselSetup /u + /i (deploys both TSF DLLs + 64-bit reg)...
"%DEST%\WeaselSetup.exe" /u
"%DEST%\WeaselSetup.exe" /i
echo     done (installer dir weasel.dll -^> SysWOW64, weaselx64.dll -^> System32)

echo [5/7] 32-bit TSF registration fallback (WeaselSetup only registers 64-bit view)...
reg query "HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}\InprocServer32" /ve 2>nul | findstr /i "weasel" >nul
if errorlevel 1 (
  echo     WOW6432Node registration missing, registering 32-bit view...
  set TEXTSERVICE_PROFILE=hans
  "%SystemRoot%\SysWOW64\regsvr32.exe" /s "%SystemRoot%\SysWOW64\weasel.dll"
  echo     done
) else (
  echo     already registered, skip
)

echo [6/7] Starting WeaselServer...
start "" "%DEST%\WeaselServer.exe"

echo [7/7] Inserting LLM config into scheme...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy_llm_schema.ps1"

echo.
echo ==== Deploy complete! ====
echo 1. REBOOT to activate System32/SysWOW64 TSF components
echo 2. Tray icon - Redeploy (rebuilds dict build with LLM librime - REQUIRED,
echo    official redeploy overwrites it with official format and 1-code chars
echo    come up empty)
echo 3. Run verify_deploy.bat to check all md5
echo 4. Verify: WPS = AI-hist mark, others = AI-TSF mark
echo    Log: %%APPDATA%%\Rime\rime_llm_filter_log.txt
pause
