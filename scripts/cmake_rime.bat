@echo off
REM cmake_rime.bat - configure librime build tree (Release, LLM filter deps)
REM Run once (or after deps change). Generates librime\build\*.sln.
REM Deps resolution: uses deps\prebuilt\ if present (project-local prebuilt libs),
REM otherwise assumes librime official build.bat flow (submodules built and
REM installed into librime\ itself). Override either with env RIME_DEPS.
REM LLAMA_ROOT: env var or -DLLAMA_ROOT; default D:/llama.cpp-mirror.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set ROOT=%~dp0..
if "%RIME_DEPS%"=="" (
  if exist "%ROOT%\deps\prebuilt\lib\leveldb.lib" (
    set RIME_DEPS=%ROOT%\deps\prebuilt
  ) else (
    set RIME_DEPS=%ROOT%\librime
  )
)
if "%BOOST_ROOT%"=="" set BOOST_ROOT=%ROOT%\deps\boost_1_84_0
if "%LLAMA_ROOT%"=="" set LLAMA_ROOT=D:/llama.cpp-mirror
cd /d %ROOT%\librime
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_TEST=OFF -DBUILD_STATIC=ON -DENABLE_LOGGING=OFF -DENABLE_EXTERNAL_PLUGINS=OFF ^
  -DBOOST_ROOT="%BOOST_ROOT%" ^
  -DLLAMA_ROOT="%LLAMA_ROOT%" ^
  -DCMAKE_PREFIX_PATH="%RIME_DEPS%" ^
  -DLevelDb_INCLUDE_PATH="%RIME_DEPS%\include" -DLevelDb_LIBRARY="%RIME_DEPS%\lib\leveldb.lib" ^
  -DMarisa_INCLUDE_PATH="%RIME_DEPS%\include" -DMarisa_LIBRARY="%RIME_DEPS%\lib\marisa.lib" ^
  -DOpencc_INCLUDE_PATH="%RIME_DEPS%\include" -DOpencc_LIBRARY="%RIME_DEPS%\lib\opencc.lib" ^
  -DOpencc_DICT_DIR="%RIME_DEPS%\share\opencc" ^
  -DYamlCpp_INCLUDE_PATH="%RIME_DEPS%\include" -DYamlCpp_LIBRARY="%RIME_DEPS%\lib\yaml-cpp.lib"
exit /b %errorlevel%
