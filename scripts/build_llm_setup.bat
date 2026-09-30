@echo off
REM build_llm_setup.bat - WeaselLLMSetup.exe x64 Release (cl direct build, no vcxproj)
REM LLM rerank settings GUI: reads/writes the llm_rerank section of the selected
REM   %APPDATA%\Rime\*.schema.yaml, then triggers a re-deploy (config lives in
REM   the scheme since 2026-09-30 - the global llm_rerank.yaml is gone).
REM Manifest: explicit (v6 common controls + system DPI awareness) - see
REM   weasel\WeaselLLMSetup\WeaselLLMSetup.manifest. Without the DPI setting the
REM   dialog is bitmap-stretched on scaled displays (blurry dark edges).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /utf-8 /O2 /W3 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   "%~dp0..\weasel\WeaselLLMSetup\WeaselLLMSetup.cpp" ^
   /Fe:"%~dp0..\bin\WeaselLLMSetup.exe" /Fo:"%TEMP%\llm_setup.obj" ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
   /MANIFESTINPUT:"%~dp0..\weasel\WeaselLLMSetup\WeaselLLMSetup.manifest" ^
   /MANIFESTDEPENDENCY:"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'" ^
   user32.lib gdi32.lib comdlg32.lib shell32.lib ole32.lib
exit /b %errorlevel%
