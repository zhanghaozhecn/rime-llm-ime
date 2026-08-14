@echo off
REM build_boost32.bat - build 32-bit (x86) boost static release libs for Win32 weasel build
REM Official naming: architecture=x86 address-model=32 -> libs with -x32- suffix
REM Only builds the variant weasel links: vc143-mt-s (link=static runtime-link=static
REM threading=multi variant=release) - much faster than --build-type=complete
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d %~dp0..\deps\boost_1_84_0
if not exist .\b2.exe call .\bootstrap.bat vc143
.\b2.exe -j%NUMBER_OF_PROCESSORS% ^
  architecture=x86 address-model=32 ^
  --with-filesystem --with-json --with-locale --with-regex ^
  --with-serialization --with-system --with-thread ^
  define=BOOST_USE_WINAPI_VERSION=0x0603 ^
  toolset=msvc-14.3 ^
  link=static runtime-link=static threading=multi variant=release ^
  stage
exit /b %errorlevel%
