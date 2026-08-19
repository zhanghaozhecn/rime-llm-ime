@echo off
setlocal EnableDelayedExpansion
REM ============================================
REM  rime-llm-ime deploy verification (run on the TARGET machine)
REM  Checks install dir components + System32 TSF + registry + server
REM  Expected values are auto-compared against the SOURCE files in this
REM  script's own directory (the deploy package) -- always in sync with
REM  the latest bin. Reference md5 (2026-08-18 round 2 build):
REM    weaselx64.dll    8C19C79F
REM    WeaselServer.exe 646DF11D
REM    rime.dll         56ADE261
REM ============================================

set CLSID={A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}
set SRC=%~dp0

echo ================================================
echo [1/5] Install dir components (vs source package)
echo ================================================
for %%D in ("C:\Program Files\Rime\weasel-0.17.4" "C:\Program Files (x86)\Rime\weasel-0.17.4") do (
  if exist "%%~D\rime.dll" (
    echo   Found install dir: %%~D
    set DEST=%%~D
    goto :found
  )
)
echo   [FAIL] install dir not found. Expected C:\Program Files\Rime\weasel-0.17.4
goto :reg

:found
for %%F in (weaselx64.dll WeaselServer.exe rime.dll) do (
  if exist "%SRC%%%F" (
    powershell -NoProfile -Command "$a=(Get-FileHash -Algorithm MD5 -Path (Join-Path '%SRC%' '%%F')).Hash; $b=(Get-FileHash -Algorithm MD5 -Path (Join-Path '!DEST!' '%%F')).Hash; if($a -eq $b){Write-Host ('  %%F  OK   '+$a)}else{Write-Host ('  [FAIL] %%F  install='+$b+'  src='+$a)}"
  ) else (
    echo   [SKIP] source file missing in this folder: %%F
  )
)

:reg
echo.
echo ================================================
echo [2/5] System32 TSF component (vs source package)
echo ================================================
if exist "C:\Windows\System32\weasel.dll" (
  powershell -NoProfile -Command "$a=(Get-FileHash -Algorithm MD5 -Path (Join-Path '%SRC%' 'weaselx64.dll')).Hash; $b=(Get-FileHash -Algorithm MD5 -Path 'C:\Windows\System32\weasel.dll').Hash; if($a -eq $b){Write-Host ('  System32\weasel.dll OK   '+$a)}else{Write-Host ('  [FAIL] System32\weasel.dll '+$b+'  src='+$a)}"
  echo   NOTE: System32 replace takes effect only AFTER REBOOT. If this FAILs
  echo         but the file was copied, reboot first and re-check.
) else (
  echo   [FAIL] System32\weasel.dll missing!
)

echo.
echo ================================================
echo [3/5] TSF registry InprocServer32
echo ================================================
reg query "HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32" /ve 2>nul | findstr /i "weasel"
if errorlevel 1 (
  reg query "HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\%CLSID%\InprocServer32" /ve 2>nul | findstr /i "weasel"
  if errorlevel 1 echo   [FAIL] InprocServer32 not found (TSF registration lost)
) else (
  echo   NOTE: should point to C:\WINDOWS\system32\weasel.dll (or a valid path)
)

echo.
echo ================================================
echo [4/5] LLM server running?
echo ================================================
tasklist /fi "imagename eq WeaselServer.exe" 2>nul | findstr /i "WeaselServer" >nul
if errorlevel 1 (echo   [FAIL] WeaselServer.exe not running) else (echo   WeaselServer.exe running)

echo.
echo ================================================
echo [5/5] Post-deploy guidance
echo ================================================
echo   After file replacement you MUST:
echo     1. REBOOT (activates System32 TSF component)
echo     2. Tray icon - Redeploy (rebuild dict build with LLM librime;
echo        official redeploy overwrites build with official format and
echo        1-code chars come up empty)
echo     3. Verify in WPS: first candidate comment shows "AI-hist"
echo        (WPS blacklist active) / in Word or Notepad: "AI-TSF"
echo     4. Check log: %APPDATA%\Rime\rime_llm_filter_log.txt
echo.
echo  If WPS still fails while all md5 above are OK and steps 1-2 done:
echo   - check whether the WPS build is 32-bit (WPS normally is 32-bit and
echo     uses SysWOW64\weasel.dll = official 32-bit TSF, which CANNOT do
echo     TSF context collection; the LLM rime.dll still re-ranks with
echo     commit-history fallback - typing itself should work)
echo   - if typing is completely broken in ALL apps: dict build mismatch
echo     (redo tray Redeploy), or older WeaselServer.exe from a previous
echo     official version is still in the install dir
pause
