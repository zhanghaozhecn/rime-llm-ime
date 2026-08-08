@echo off
REM ============================================
REM  rime-llm-ime one-click deploy script
REM  Place this file in bin\ and double-click it
REM  Auto-elevates to admin, copies LLM files
REM  Restart the system after running
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

echo [1/4] Stopping WeaselServer...
taskkill /f /im WeaselServer.exe >nul 2>&1
timeout /t 3 /nobreak >nul

echo [2/4] Copying LLM files to install dir...
copy /y "%SRC%rime.dll" "%DEST%\" >nul
copy /y "%SRC%weaselx64.dll" "%DEST%\" >nul
copy /y "%SRC%WeaselServer.exe" "%DEST%\" >nul
copy /y "%SRC%WeaselDeployer.exe" "%DEST%\" >nul
copy /y "%SRC%opencc.dll" "%DEST%\" >nul
copy /y "%SRC%vcomp140.dll" "%DEST%\" >nul
echo     done (rime.dll / weaselx64.dll / WeaselServer / WeaselDeployer / opencc / vcomp140)

echo [3/4] System32 TSF delayed replace (takes effect after reboot, keeps TSF registration)...
powershell -Command "Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public class M{[DllImport(\"kernel32.dll\",SetLastError=true,CharSet=CharSet.Unicode)]public static extern bool MoveFileEx(string a,string b,int f);}'; Copy-Item -Path '%SRC%weaselx64.dll' -Destination 'C:\Windows\System32\weasel.dll.new' -Force; if (-not [M]::MoveFileEx('C:\Windows\System32\weasel.dll.new','C:\Windows\System32\weasel.dll',5)) { Write-Host ('MoveFileEx failed: ' + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()); exit 1 }"
if %errorlevel% neq 0 (
  echo     [WARNING] delayed replace failed, copy weaselx64.dll to C:\Windows\System32\weasel.dll manually
)

echo [4/4] Starting WeaselServer...
start "" "%DEST%\WeaselServer.exe"

echo.
echo ==== Deploy complete! Please reboot to activate the System32 component ====
echo Verify after reboot: first candidate shows gold AI-TSF / AI-hist mark
pause
