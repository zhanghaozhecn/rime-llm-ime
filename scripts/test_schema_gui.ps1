# test_schema_gui.ps1 — WeaselLLMSetup 方案接入 GUI 自动化测试
#
# 隔离：测试期间 $env:APPDATA 重定向到沙箱目录（子进程 GUI 继承），
# 方案与 llm_rerank.yaml 读写全落沙箱——物理杜绝误伤活配置。
# （2026-09-29 事故教训：无隔离 + CB_FINDSTRING ANSI marshal 失效 =
# 下拉停留默认第一项 = 活方案 pdsp.schema.yaml；已从 .source 恢复。）
# 结构照搬已验证可行的分步手动调试（单消息类 + 顶层顺序 + 固定等待）。
$ErrorActionPreference = "Stop"
$exe = "D:\rime-llm-ime\bin\WeaselLLMSetup.exe"
$sandbox = Join-Path $env:TEMP ("llmsetup_sandbox_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
$test = Join-Path $sandbox "Rime\zz_test_gui.schema.yaml"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
}
"@

function Get-Hwnd {
  for ($t = 0; $t -lt 10; $t++) {
    $hw = (Get-Process WeaselLLMSetup -ErrorAction SilentlyContinue).MainWindowHandle
    if ($hw) { return $hw }
    Start-Sleep -Seconds 1
  }
  throw "window not found"
}
function Read-Stat($hw) {
  $sb = New-Object System.Text.StringBuilder 512
  [W]::GetWindowText([W]::GetDlgItem($hw, 1035), $sb, 512) | Out-Null
  return $sb.ToString()
}
function Click-Add($hw) {
  [W]::SendMessage($combo, 0x014E, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # CB_SETCURSEL(0)
  [W]::SendMessage([W]::GetDlgItem($hw, 1033), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 900
  return (Read-Stat $hw)
}
function Click-Remove($hw) {
  [W]::SendMessage([W]::GetDlgItem($hw, 1034), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 900
  return (Read-Stat $hw)
}
function Show-File {
  Get-Content $test -Encoding UTF8 | ForEach-Object -Begin { $i = 1 } -Process { Write-Host ("    {0}: {1}" -f $i, $_); $i++ }
}
$script:fail = 0
function Assert([string]$name, [bool]$cond) {
  if ($cond) { Write-Host ("  [PASS] " + $name) }
  else { $script:fail++; Write-Host ("  [FAIL] " + $name) }
}

New-Item -ItemType Directory -Path (Join-Path $sandbox "Rime") -Force | Out-Null
$env:APPDATA = $sandbox                       # 子进程 GUI 继承 → 读写全落沙箱
$env:WEASEL_LLM_SETUP_NO_REDEPLOY = "1"       # 静默跳过重新部署（测试开关）
try {
  @'
schema:
  schema_id: zz_test
engine:
  processors:
    - ascii_composer
  filters:
    - simplifier
    - uniquifier
    - pin_fix_filter
'@ | Out-File -FilePath $test -Encoding ascii
  Start-Process -FilePath $exe
  Start-Sleep -Seconds 3
  $hw = Get-Hwnd
  $combo = [W]::GetDlgItem($hw, 1031)

  Write-Host "== phase1: 干净方案接入 =="
  $st = Click-Add $hw
  Write-Host ("  status: " + $st)
  $f = Get-Content $test -Encoding UTF8
  $iU = 0; for ($i = 0; $i -lt $f.Count; $i++) { if ($f[$i] -match "uniquifier") { $iU = $i } }
  Assert "状态行含'已接入'" ($st -match "已接入")
  Assert "llm_filter 恰 1 行" ((($f | Select-String "llm_filter").Count) -eq 1)
  Assert "插在 uniquifier 后" ($f[$iU + 1] -match "llm_filter")

  Write-Host "== phase2: 幂等重跑 =="
  $st = Click-Add $hw
  $f = Get-Content $test -Encoding UTF8
  Assert "仍恰 1 行" ((($f | Select-String "llm_filter").Count) -eq 1)

  Write-Host "== phase3: 插件版组件跨版转换 =="
  @'
schema:
  schema_id: zz_test
engine:
  processors:
    - lua_processor@*llm_processor
    - ascii_composer
  filters:
    - simplifier
    - uniquifier
    - lua_filter@*llm_filter

llm_rerank:
  enabled: true
  min_code_len: 4
'@ | Out-File -FilePath $test -Encoding ascii
  $st = Click-Add $hw
  Write-Host ("  status: " + $st)
  $f = Get-Content $test -Encoding UTF8
  Assert "插件组件行已剥" ((($f | Select-String "lua_processor@|lua_filter@").Count) -eq 0)
  Assert "llm_rerank 节已剥" ((($f | Select-String "^llm_rerank:").Count) -eq 0)
  Assert "llm_filter 恰 1 行" ((($f | Select-String "llm_filter").Count) -eq 1)
  Show-File

  Write-Host "== phase4: 剥离 =="
  $st = Click-Remove $hw
  Write-Host ("  status: " + $st)
  $f = Get-Content $test -Encoding UTF8
  Assert "llm_filter 已无" ((($f | Select-String "llm_filter").Count) -eq 0)

  Write-Host "== phase5: simplifier 回退 =="
  @'
schema:
  schema_id: zz_test
engine:
  filters:
    - simplifier
    - reverse_lookup_filter
'@ | Out-File -FilePath $test -Encoding ascii
  $st = Click-Add $hw
  Write-Host ("  status: " + $st)
  $f = Get-Content $test -Encoding UTF8
  $iS = 0; for ($i = 0; $i -lt $f.Count; $i++) { if ($f[$i] -match "simplifier") { $iS = $i } }
  Assert "插在 simplifier 后" ($f[$iS + 1] -match "llm_filter")

  Write-Host "== phase6: 再剥离（第一次移除 phase5 的行，第二次才'未发现'）=="
  $st = Click-Remove $hw
  Assert "第一次移除成功" ($st -match "已移除")
  $st = Click-Remove $hw
  Assert "重复剥离提示未发现" ($st -match "未发现")
  Write-Host ("  status: " + $st)
}
finally {
  Get-Process WeaselLLMSetup -ErrorAction SilentlyContinue | Stop-Process -Force
  Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:fail -eq 0) { Write-Host "`nALL PASS" } else { Write-Host "`nFAILED: $($script:fail)"; exit 1 }
