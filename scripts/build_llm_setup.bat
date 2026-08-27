@echo off
REM build_llm_setup.bat - WeaselLLMSetup.exe x64 Release (cl direct build, no vcxproj)
REM LLM rerank settings GUI: reads/writes %APPDATA%\Rime\llm_rerank.yaml (hot reload).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /utf-8 /O2 /W3 /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   "%~dp0..\weasel\WeaselLLMSetup\WeaselLLMSetup.cpp" ^
   /Fe:"%~dp0..\bin\WeaselLLMSetup.exe" /Fo:"%TEMP%\llm_setup.obj" ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comdlg32.lib shell32.lib
exit /b %errorlevel%
