@echo off
REM run_test_rime.bat - rime.dll smoke test (bin rime vs installer rime)
REM test_rime.exe must run from bin dir (DLL dependency resolution)
"%~dp0..\bin\test_rime.exe" "%~dp0..\bin\rime.dll" > "%~dp0..\test_new.txt" 2>&1
echo NEW exit=%errorlevel%
"%~dp0..\bin\test_rime.exe" "C:\Program Files\Rime\weasel-0.17.4\rime.dll" > "%~dp0..\test_old.txt" 2>&1
echo OLD exit=%errorlevel%
