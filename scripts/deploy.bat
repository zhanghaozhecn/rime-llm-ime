@echo off
taskkill /f /im WeaselServer.exe >nul 2>&1
timeout /t 2 /nobreak >nul
set DEST=C:\Program Files\Rime\weasel-0.17.4
set SRC=D:\rime-build\weasel_out
set NEWRIME=D:\rime-build\librime-master\build\bin\Release
ren "C:\WINDOWS\System32\weasel.dll" "weasel.dll.old%RANDOM%"
copy /y "%SRC%\weaselx64.dll" "C:\WINDOWS\System32\weasel.dll"
copy /y "%SRC%\WeaselServer.exe" "%DEST%\" >nul
copy /y "%NEWRIME%\rime.dll" "%DEST%\" >nul
copy /y "%SRC%\weaselx64.dll" "%DEST%\" >nul
echo ==== done ====
pause
start "" "%DEST%\WeaselServer.exe"
