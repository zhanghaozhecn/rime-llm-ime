@echo off
REM build_setup.bat - WeaselSetup.exe Win32 Release (TSF registration tool, fresh-machine install path)
REM Win32 per weasel.sln config mapping (x64 solution -> Win32 project).
REM SolutionDir passed explicitly and unquoted: direct vcxproj build leaves it
REM undefined (WTL include paths depend on it), and a quoted trailing backslash
REM would escape the closing quote (path has no spaces).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild "%~dp0..\weasel\WeaselSetup\WeaselSetup.vcxproj" /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir=%~dp0..\weasel\ /v:minimal /nologo
exit /b %errorlevel%
