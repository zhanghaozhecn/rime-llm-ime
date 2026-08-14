# tsf_switch.ps1 — 不重启切换 TSF 组件加载路径（测试用）
# 原理：TSF 组件是每进程按需加载。把注册表 InprocServer32 临时指向
# 新编译的 weaselx64.dll → 新启动的应用加载新 DLL，已运行进程不受影响。
# 测试完 -Restore 恢复 System32 官方路径。无需重启系统。
#
# 用法（管理员）：
#   pwsh -File tsf_switch.ps1 -Test [-Dll <weaselx64.dll 路径，默认 weasel\output\weaselx64.dll>]
#   pwsh -File tsf_switch.ps1 -Restore
#   pwsh -File tsf_switch.ps1 -Status

param(
  [switch]$Test,
  [switch]$Restore,
  [switch]$Status,
  [string]$Dll = (Join-Path (Split-Path $PSScriptRoot -Parent) 'weasel\output\weaselx64.dll')
)

$ErrorActionPreference = "Stop"

# 自动提权（写注册表需要管理员；-Status 只读不需要）
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $Status) {
  $argList = @("-NoProfile", "-File", "`"$PSCommandPath`"")
  if ($Test) { $argList += "-Test" }
  if ($Restore) { $argList += "-Restore" }
  if ($Dll) { $argList += "-Dll"; $argList += "`"$Dll`"" }
  Start-Process -FilePath "pwsh" -ArgumentList $argList -Verb RunAs -Wait
  exit 0
}

$key = "HKLM:\SOFTWARE\Classes\CLSID\{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}\InprocServer32"
$stateFile = Join-Path $env:TEMP "tsf_switch_original.txt"

function Get-CurrentPath {
  (Get-ItemProperty -Path $key -Name "(default)" -ErrorAction SilentlyContinue)."(default)"
}

if ($Status) {
  Write-Host ("当前 InprocServer32: " + (Get-CurrentPath))
  if (Test-Path $stateFile) {
    Write-Host ("备份原路径: " + (Get-Content $stateFile))
  } else {
    Write-Host "无备份状态文件"
  }
  exit 0
}

if ($Test) {
  $resolved = (Resolve-Path $Dll -ErrorAction SilentlyContinue)
  if (-not $resolved) {
    Write-Host "[ERROR] DLL 不存在: $Dll" -ForegroundColor Red
    exit 1
  }
  $current = Get-CurrentPath
  if ($current -eq $resolved.Path) {
    Write-Host "已指向测试 DLL，无需切换: $($resolved.Path)" -ForegroundColor Yellow
    exit 0
  }
  # 备份原路径（只备份一次，防止连续 Test 覆盖真值）
  if (-not (Test-Path $stateFile)) {
    Set-Content -Path $stateFile -Value $current -Encoding ASCII
  }
  Set-ItemProperty -Path $key -Name "(default)" -Value $resolved.Path
  Write-Host ("已切换: " + $current + " → " + $resolved.Path) -ForegroundColor Green
  Write-Host "提示：已运行的应用仍用旧 DLL；请关闭后重新打开测试应用（记事本等）。"
  Write-Host "      测试完执行: pwsh -File tsf_switch.ps1 -Restore"
  exit 0
}

if ($Restore) {
  $orig = $null
  if (Test-Path $stateFile) {
    $orig = (Get-Content $stateFile).Trim()
  }
  if (-not $orig) { $orig = "C:\WINDOWS\system32\weasel.dll" }
  Set-ItemProperty -Path $key -Name "(default)" -Value $orig
  Remove-Item $stateFile -Force -ErrorAction SilentlyContinue
  Write-Host ("已恢复: " + $orig) -ForegroundColor Green
  Write-Host "提示：已运行的应用仍用测试 DLL；关闭重开应用后回到官方组件。"
  exit 0
}

Write-Host "用法: -Test [-Dll 路径] | -Restore | -Status"
exit 2
