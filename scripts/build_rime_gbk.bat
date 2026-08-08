@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild "D:\rime-build\librime-master\build\src\rime.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal
