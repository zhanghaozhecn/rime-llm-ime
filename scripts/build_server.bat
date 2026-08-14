@echo off
REM build_server.bat - WeaselServer x64 Release build
REM Prereq: weasel.props (from template) + rime.lib in weasel\lib64 (see gen_rime_lib.bat)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild "%~dp0..\weasel\weasel.sln" /p:Configuration=Release /p:Platform=x64 /t:WeaselServer /v:minimal /nologo
exit /b %errorlevel%
