@echo off
REM ============================================
REM  rime-llm-ime deploy verification (run on the TARGET machine)
REM  Checks: install dir components, System32 TSF, registry, guidance
REM  Expected md5 (0.17.4-base final build, 2026-08-12):
REM    weaselx64.dll    18f39c9732d2d5d4ddec7cd2c173ad9a  TSF + LLM ctx + WPS blacklist
REM    WeaselServer.exe 3c9229814fef350e4cc50da7a2695546  server + SET_CONTEXT_TEXT IPC
REM    rime.dll         97c6343dadd3932e758bc702dcabc534  librime + llm_filter
REM    opencc.dll       0d9f9b2a1526d720fbe7a77636a0e831
REM    vcomp140.dll     a0e2cc1573537419a7b9327f9062d448
REM    WeaselDeployer   4f26c2c7723e12d2a7e5c50cd5709d90
REM ============================================

set EXPECT_TF=18f39c9732d2d5d4ddec7cd2c173ad9a
set EXPECT_SV=3c9229814fef350e4cc50da7a2695546
set EXPECT_RL=97c6343dadd3932e758bc702dcabc534
set CLSID={A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}

echo ================================================
echo [1/5] Install dir components
echo ================================================
for %%D in ("C:\Program Files\Rime\weasel-0.17.4" "C:\Program Files (x86)\Rime\weasel-0.17.4") do (
  if exist "%%~D\rime.dll" (
    echo   Found install dir: %%~D
    set DEST=%%~D
    goto :found
  )
)
echo   [FAIL] install dir not found. Expected C:\Program Files\Rime\weasel-0.17.4
echo          (deploy_llm.bat hard-codes this path; if the target machine
echo           installed another version, files were NOT copied)
goto :reg

:found
echo   checking md5...
for /f "delims=" %%h in ('certutil -hashfile "%DEST%\weaselx64.dll" MD5 ^| findstr /i /v "CertUtil hashfile MD5"') do set TF=%%h
for /f "delims=" %%h in ('certutil -hashfile "%DEST%\WeaselServer.exe" MD5 ^| findstr /i /v "CertUtil hashfile MD5"') do set SV=%%h
for /f "delims=" %%h in ('certutil -hashfile "%DEST%\rime.dll" MD5 ^| findstr /i /v "CertUtil hashfile MD5"') do set RL=%%h
if /i "%TF%"=="%EXPECT_TF%" (echo   weaselx64.dll    OK   %TF%) else (echo   [FAIL] weaselx64.dll    %TF% ^<^> expected %EXPECT_TF%)
if /i "%SV%"=="%EXPECT_SV%" (echo   WeaselServer.exe OK   %SV%) else (echo   [FAIL] WeaselServer.exe %SV% ^<^> expected %EXPECT_SV%)
if /i "%RL%"=="%EXPECT_RL%" (echo   rime.dll         OK   %RL%) else (echo   [FAIL] rime.dll         %RL% ^<^> expected %EXPECT_RL%)

:reg
echo.
echo ================================================
echo [2/5] System32 TSF component
echo ================================================
if exist "C:\Windows\System32\weasel.dll" (
  for /f "delims=" %%h in ('certutil -hashfile "C:\Windows\System32\weasel.dll" MD5 ^| findstr /i /v "CertUtil hashfile MD5"') do set SYS=%%h
  if /i "%SYS%"=="%EXPECT_TF%" (echo   System32\weasel.dll OK   %SYS%) else (echo   [FAIL] System32\weasel.dll %SYS% ^<^> expected %EXPECT_TF%)
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
