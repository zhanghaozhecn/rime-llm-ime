@echo off
REM ============================================
REM  rime-llm-ime 一键部署脚本（普通用户）
REM  放在 bin\ 目录下，双击运行即可
REM  自动请求管理员权限，复制 LLM 版文件
REM  完成后请重启系统（System32 组件延迟替换生效）
REM ============================================

REM 自动请求管理员权限
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo 正在请求管理员权限，请确认 UAC 弹窗...
  powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

set DEST=C:\Program Files\Rime\weasel-0.17.4
set SRC=%~dp0

if not exist "%DEST%" (
  echo [错误] 未找到小狼毫安装目录: %DEST%
  echo 请先安装官方小狼毫 0.17.4 后再运行本脚本
  pause
  exit /b 1
)

echo [1/4] 停止 WeaselServer...
taskkill /f /im WeaselServer.exe >nul 2>&1
timeout /t 3 /nobreak >nul

echo [2/4] 复制 LLM 版文件到安装目录...
copy /y "%SRC%rime.dll" "%DEST%\" >nul
copy /y "%SRC%weaselx64.dll" "%DEST%\" >nul
copy /y "%SRC%WeaselServer.exe" "%DEST%\" >nul
copy /y "%SRC%WeaselDeployer.exe" "%DEST%\" >nul
copy /y "%SRC%opencc.dll" "%DEST%\" >nul
copy /y "%SRC%vcomp140.dll" "%DEST%\" >nul
echo     完成（rime.dll / weaselx64.dll / WeaselServer / WeaselDeployer / opencc / vcomp140）

echo [3/4] System32 TSF 组件延迟替换（重启后生效，不破坏 TSF 注册）...
powershell -Command "Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public class M{[DllImport(\"kernel32.dll\",SetLastError=true,CharSet=CharSet.Unicode)]public static extern bool MoveFileEx(string a,string b,int f);}'; Copy-Item -Path '%SRC%weaselx64.dll' -Destination 'C:\Windows\System32\weasel.dll.new' -Force; if (-not [M]::MoveFileEx('C:\Windows\System32\weasel.dll.new','C:\Windows\System32\weasel.dll',5)) { Write-Host ('MoveFileEx failed: ' + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()); exit 1 }"
if %errorlevel% neq 0 (
  echo     [警告] 延迟替换失败，请手动复制 weaselx64.dll 到 C:\Windows\System32\weasel.dll
)

echo [4/4] 启动 WeaselServer...
start "" "%DEST%\WeaselServer.exe"

echo.
echo ==== 部署完成！请重启系统使 System32 组件生效 ====
echo 重启后验证：打字时首选候选带 AI·TSF / AI·历史 金色标记
pause
