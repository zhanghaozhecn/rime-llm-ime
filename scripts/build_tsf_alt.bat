@echo off
REM build_tsf_alt.bat - WeaselTSF build to alternate output dir (original locked by running hosts)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild "D:\llm-rerank\rime-llm-ime\weasel\weasel.sln" /p:Configuration=Release /p:Platform=x64 /t:WeaselTSF /p:OutDir=D:\llm-rerank\rime-llm-ime\weasel\output_tsffix\ /v:minimal /nologo
exit /b %errorlevel%
