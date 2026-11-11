@echo off
REM build_llm_debug.bat - cl direct build of WeaselLLMDebug.exe
REM (LLM live debug window; pattern copied from build_llm_setup.bat)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cl /nologo /utf-8 /O2 /W3 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   "%~dp0..\weasel\WeaselLLMDebug\WeaselLLMDebug.cpp" ^
   /Fe:"%~dp0..\bin\WeaselLLMDebug.exe" /Fo:"%TEMP%\llm_debug.obj" ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
   /MANIFESTINPUT:"%~dp0..\weasel\WeaselLLMDebug\WeaselLLMDebug.manifest" ^
   /MANIFESTDEPENDENCY:"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'" ^
   user32.lib gdi32.lib
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK
