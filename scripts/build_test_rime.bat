@echo off
REM build_test_rime.bat - compile test_rime.cpp smoke test (needs rime.lib in weasel\lib64)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /utf-8 /I "%~dp0..\weasel\librime\include" "%~dp0test_rime.cpp" "%~dp0..\weasel\lib64\rime.lib" /Fe:%~dp0test_rime.exe
exit /b %errorlevel%
