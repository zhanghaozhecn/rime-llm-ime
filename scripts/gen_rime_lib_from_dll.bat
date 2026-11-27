@echo off
REM gen_rime_lib_from_dll.bat - regenerate weasel\lib64\rime.lib from the current rime.dll
REM (import-by-name lib; rebuild after adding new RIME_API exports)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
dumpbin /exports "..\librime\build\bin\Release\rime.dll" > "%TEMP%\rime_exports.txt"
python gen_rime_def.py "%TEMP%\rime_exports.txt" rime.def
if errorlevel 1 exit /b 1
lib /def:rime.def /machine:x64 /out:"%~dp0..\weasel\lib64\rime.lib" >nul
echo lib staged: weasel\lib64\rime.lib
exit /b %errorlevel%
