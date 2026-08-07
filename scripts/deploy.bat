@echo off
echo [1/4] stopping WeaselServer...
taskkill /f /im WeaselServer.exe >nul 2>&1
timeout /t 2 /nobreak >nul
set DEST=C:\Program Files\Rime\weasel-0.17.4
set SRC=D:\rime-build\weasel_out
set NEWRIME=D:\rime-build\librime-master\build\bin\Release
echo [2/4] updating System32\weasel.dll (TSF)...
ren "C:\WINDOWS\System32\weasel.dll" "weasel.dll.old%RANDOM%"
copy /y "%SRC%\weaselx64.dll" "C:\WINDOWS\System32\weasel.dll"
echo [3/4] updating %DEST% + sync source (weasel_out)...
copy /y "%SRC%\WeaselServer.exe" "%DEST%\"
copy /y "%NEWRIME%\rime.dll" "%DEST%\"
copy /y "%NEWRIME%\rime.dll" "%SRC%\"
copy /y "%SRC%\weaselx64.dll" "%DEST%\"
echo [4/4] restarting WeaselServer...
echo ==== done ====
pause
start "" "%DEST%\WeaselServer.exe"
