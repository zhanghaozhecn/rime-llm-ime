@echo off
REM ============================================
REM  rime-llm-ime one-click deploy script
REM  0.17.4-base build (2026-08-12):
REM    weaselx64.dll  18f39c97  TSF: 0.17.4 + LLM ctx + WPS blacklist
REM    WeaselServer   3c922981  server: 0.17.4 + SET_CONTEXT_TEXT/RESET_CONTEXT IPC
REM    rime.dll       97c6343d  librime + llm_filter
REM  Behavior: WPS=commit history rerank (AI-hist), others=TSF ctx (AI-TSF)
REM  Place this file in bin\ and double-click it
REM  Auto-elevates to admin, copies LLM files
REM  Reboot + tray redeploy after running (see notes below)
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

echo [1/6] Checking LLM model...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy_llm_model.ps1"

echo [2/6] Stopping WeaselServer...
taskkill /f /im WeaselServer.exe >nul 2>&1
timeout /t 3 /nobreak >nul

echo [3/6] Copying LLM files to install dir...
copy /y "%SRC%rime.dll" "%DEST%\" >nul
copy /y "%SRC%weaselx64.dll" "%DEST%\" >nul
copy /y "%SRC%WeaselServer.exe" "%DEST%\" >nul
copy /y "%SRC%WeaselDeployer.exe" "%DEST%\" >nul
copy /y "%SRC%opencc.dll" "%DEST%\" >nul
copy /y "%SRC%vcomp140.dll" "%DEST%\" >nul
echo     done (rime.dll / weaselx64.dll / WeaselServer / WeaselDeployer / opencc / vcomp140)

echo [4/6] System32 TSF delayed replace (takes effect after reboot, keeps TSF registration)...
powershell -Command "Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public class M{[DllImport(\"kernel32.dll\",SetLastError=true,CharSet=CharSet.Unicode)]public static extern bool MoveFileEx(string a,string b,int f);}'; Copy-Item -Path '%SRC%weaselx64.dll' -Destination 'C:\Windows\System32\weasel.dll.new' -Force; if (-not [M]::MoveFileEx('C:\Windows\System32\weasel.dll.new','C:\Windows\System32\weasel.dll',5)) { Write-Host ('MoveFileEx failed: ' + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()); exit 1 }"
if %errorlevel% neq 0 (
  echo     [WARNING] delayed replace failed, copy weaselx64.dll to C:\Windows\System32\weasel.dll manually
)

echo [5/6] Starting WeaselServer...
start "" "%DEST%\WeaselServer.exe"

echo [6/6] Inserting LLM config into scheme...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy_llm_schema.ps1"

echo.
echo ==== Deploy complete! ====
echo 1. Reboot to activate the System32 TSF component
echo 2. Tray icon - Redeploy (rebuilds dict build with LLM librime - REQUIRED,
echo    official redeploy overwrites it with official format and 1-code chars
echo    come up empty)
echo 3. Deploy script inserted llm_filter + llm_rerank into pdsp.schema.yaml
echo    (idempotent, exact position: uniquifier after, pin_fix before)
echo 3. Verify: WPS = AI-hist mark, others = AI-TSF mark
pause
