@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set BOOST_ROOT=D:\OneDrive\typing\Rime文档源码\weasel-master\deps\boost_1_84_0
msbuild "D:\OneDrive\typing\Rime文档源码\weasel-master\weasel.sln" /p:Configuration=Release /p:Platform=x64 /t:WeaselServer /v:minimal
