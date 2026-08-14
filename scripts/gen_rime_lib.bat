@echo off
REM gen_rime_lib.bat - stage librime import lib for weasel link
REM WeaselServer/WeaselDeployer link rime.lib (searched in weasel\lib for x86,
REM weasel\lib64 for x64). Librime official build.bat produces librime\dist\lib\rime.lib.
REM If unavailable, generate manually from rime.dll:
REM   dumpbin /exports librime\build\bin\Release\rime.dll > rime.exports
REM   edit into a .def file (EXPORTS section), then:
REM   lib /def:rime.def /machine:x64 /out:weasel\lib64\rime.lib
if not exist "%~dp0..\librime\dist\lib\rime.lib" (
  echo [ERROR] librime\dist\lib\rime.lib not found - run librime\build.bat first
  exit /b 1
)
if not exist "%~dp0..\weasel\lib" mkdir "%~dp0..\weasel\lib"
if not exist "%~dp0..\weasel\lib64" mkdir "%~dp0..\weasel\lib64"
copy /y "%~dp0..\librime\dist\lib\rime.lib" "%~dp0..\weasel\lib\rime.lib" >nul
copy /y "%~dp0..\librime\dist\lib\rime.lib" "%~dp0..\weasel\lib64\rime.lib" >nul
echo rime.lib staged: weasel\lib (x86) + weasel\lib64 (x64)
exit /b 0
