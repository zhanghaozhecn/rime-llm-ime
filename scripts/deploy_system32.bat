@echo off
REM deploy_system32.bat - deploy latest 64-bit LLM weasel.dll to System32
REM Uses MoveFileEx DELAY_UNTIL_REBOOT (safe for locked DLL; takes effect on reboot)
REM Double-click to run (auto-elevates).
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -Command "Start-Process cmd -ArgumentList '/c \"%~f0\"' -Verb RunAs"
  exit /b
)
copy /y "%~dp0..\weasel\output\weaselx64.dll" "C:\WINDOWS\System32\weasel.dll.new" >nul
if errorlevel 1 (
  echo Failed to stage DLL. Aborting.
  exit /b 1
)
powershell -Command "$sig = '[DllImport(\"kernel32.dll\", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool MoveFileEx(string src, string dst, uint flags);'; $t = Add-Type -MemberDefinition $sig -Name MFE -Namespace W32 -PassThru; $r = $t::MoveFileEx('C:\WINDOWS\System32\weasel.dll.new', 'C:\WINDOWS\System32\weasel.dll', 5); if ($r) { Write-Host 'Staged: System32\weasel.dll will be replaced on next reboot.' } else { Write-Host 'MoveFileEx failed.' }"
timeout /t 2 /nobreak >nul
