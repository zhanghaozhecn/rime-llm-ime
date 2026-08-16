@echo off
rem build_test_llm_context.bat - build+run the context-logic regression test
rem (pure std::string logic, no llama/librime dependency)
rem usage: build_test_llm_context.bat  (output: test_llm_context.exe here)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d %~dp0
cl /O2 /std:c++17 /EHsc /DNDEBUG /utf-8 test_llm_context.cpp /Fe:test_llm_context.exe
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
rem explicit path: cmd may not search the current dir (NoDefaultCurrentDirectoryInExePath)
"%~dp0test_llm_context.exe"
exit /b %errorlevel%
