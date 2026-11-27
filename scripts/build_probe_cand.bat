@echo off
REM build_probe_cand.bat - compile rime_cand_probe.cpp (headless candidate probe, local tool)
REM Supports --cls (needs rime.dll exporting RimeGetCandidateCls, 2026-11-27+).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /EHsc /utf-8 /I "%~dp0..\weasel\librime\include" "%~dp0rime_cand_probe.cpp" "%~dp0..\weasel\lib64\rime.lib" /Fe:"%~dp0rime_cand_probe.exe"
exit /b %errorlevel%
