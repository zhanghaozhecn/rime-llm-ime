@echo off
REM build_tsf.bat [rebuild] - WeaselTSF x64 Release build
REM BOOST_ROOT via weasel.props (generated from weasel.props.template)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if "%1"=="rebuild" (
  msbuild "%~dp0..\weasel\weasel.sln" /p:Configuration=Release /p:Platform=x64 /t:WeaselTSF:Rebuild /v:minimal /nologo
) else (
  msbuild "%~dp0..\weasel\weasel.sln" /p:Configuration=Release /p:Platform=x64 /t:WeaselTSF /v:minimal /nologo
)
exit /b %errorlevel%
