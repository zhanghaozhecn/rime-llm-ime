# test_schema_gui.ps1 — WeaselLLMSetup 方案接入 GUI 自动化测试
#
# 隔离：测试期间 $env:APPDATA 重定向到沙箱目录（子进程 GUI 继承），
# 方案与 llm_rerank 配置节读写全落沙箱——物理杜绝误伤活配置。
# （2026-09-30 起配置写在方案 schema.yaml 的 llm_rerank 节里，无全局文件）
# （2026-09-29 事故教训：无隔离 + CB_FINDSTRING ANSI marshal 失效 =
# 下拉停留默认第一项 = 活方案 pdsp.schema.yaml；已从 .source 恢复。）
# 结构照搬已验证可行的分步手动调试（单消息类 + 顶层顺序 + 固定等待）。
$ErrorActionPreference = "Stop"
$exe = (Resolve-Path (Join-Path $PSScriptRoot "..\bin\WeaselLLMSetup.exe")).Path   # 相对本脚本定位，免绝对路径
$sandbox = Join-Path $env:TEMP ("llmsetup_sandbox_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
$test = Join-Path $sandbox "Rime\zz_test_gui.schema.yaml"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageStr(IntPtr h, uint m, IntPtr w, string s);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageBuf(IntPtr h, uint m, IntPtr w, [Out][MarshalAs(UnmanagedType.LPWStr)] System.Text.StringBuilder s);
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
function Read-Ctl($hw, [int]$id) {
  # 跨进程取子控件文本必须走 WM_GETTEXT（GetWindowText 对无 caption 的子控件
  # 跨进程返回空串——2026-09-30 phase8 实测：STATIC 能读到、EDIT/COMBOBOX 全空）
  $h = [W]::GetDlgItem($hw, $id)
  if ($h -eq [IntPtr]::Zero) { return "<?>" }
  $len = [int][W]::SendMessage($h, 0x000E, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_GETTEXTLENGTH
  if ($len -le 0) { return "" }
  $sb = New-Object System.Text.StringBuilder ($len + 2)
  [void][W]::SendMessageBuf($h, 0x000D, [IntPtr]($len + 1), $sb)             # WM_GETTEXT
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
  # 2026-09-30 起：接入 = 组件行 + llm_rerank 配置节（配置回归方案，两版同款）
  Assert "llm_rerank 配置节恰 1" ((($f | Where-Object { $_ -match '^llm_rerank:' }).Count) -eq 1)
  Assert "节内 enabled: true" ((($f | Where-Object { $_ -match '^  enabled: true$' }).Count) -eq 1)

  Write-Host "== phase2: 幂等重跑 =="
  $st = Click-Add $hw
  $f = Get-Content $test -Encoding UTF8
  Assert "仍恰 1 行" ((($f | Select-String "llm_filter").Count) -eq 1)
  Assert "配置节仍恰 1（不重复插入）" ((($f | Where-Object { $_ -match '^llm_rerank:' }).Count) -eq 1)

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
  Assert "llm_filter 恰 1 行" ((($f | Select-String "llm_filter").Count) -eq 1)
  # 旧节被剥掉后按界面值重建（旧键 min_code_len 不再回流）
  Assert "配置节重建恰 1" ((($f | Where-Object { $_ -match '^llm_rerank:' }).Count) -eq 1)
  Assert "旧键 min_code_len 未回流" ((($f | Where-Object { $_ -match 'min_code_len' }).Count) -eq 0)
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

  # phase7: 参数保存 → 写进**方案文件**的 llm_rerank 配置节（2026-09-30 用户定案：
  # 两版统一回归 Rime 原生做法，全局 %APPDATA%\Rime\llm_rerank.yaml 已取消）。
  # 断言：节内容键序与插件版逐字一致 + 旧键不再产出 + 旧全局文件不再产生。
  Write-Host "== phase7: 保存参数 → 方案 llm_rerank 配置节 =="
  @'
schema:
  schema_id: zz_test
engine:
  filters:
    - simplifier
    - uniquifier
'@ | Out-File -FilePath $test -Encoding ascii
  $st = Click-Add $hw
  Write-Host ("  status: " + $st)
  $f = Get-Content $test -Encoding UTF8
  Assert "接入后含 llm_rerank 节" ((($f | Where-Object { $_ -match '^llm_rerank:' }).Count) -eq 1)
  # 写一个带特殊字符的正则（YAML 单引号风格 + 花括号）后保存
  $txt = [W]::GetDlgItem($hw, 1016)                    # IDC_CODE_PAT
  [W]::SendMessage($txt, 0x000C, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_SETTEXT ""（先清空）
  [W]::SendMessageStr($txt, 0x000C, [IntPtr]::Zero, "[abcde]{4}") | Out-Null # WM_SETTEXT 目标值
  [W]::SendMessage([W]::GetDlgItem($hw, 1101), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # IDC_SAVE
  Start-Sleep -Milliseconds 900
  $y = Get-Content $test -Encoding UTF8
  Assert "code_pattern 单引号写出 [abcde]{4}" (($y | Where-Object { $_ -match "^  code_pattern: '\[abcde\]\{4\}'$" }).Count -eq 1)
  Assert "enabled: true" (($y | Where-Object { $_ -match '^  enabled: true$' }).Count -eq 1)
  Assert "max_tokens: 10" (($y | Where-Object { $_ -match '^  max_tokens: 10$' }).Count -eq 1)
  Assert "max_candidates: 5" (($y | Where-Object { $_ -match '^  max_candidates: 5$' }).Count -eq 1)
  Assert "cpu_cores: 4" (($y | Where-Object { $_ -match '^  cpu_cores: 4$' }).Count -eq 1)
  Assert "freq_beta: 1.50" (($y | Where-Object { $_ -match '^  freq_beta: 1\.50$' }).Count -eq 1)
  Assert "expected_length_weight: 0.20" (($y | Where-Object { $_ -match '^  expected_length_weight: 0\.20$' }).Count -eq 1)
  Assert "debug_fusion: false" (($y | Where-Object { $_ -match '^  debug_fusion: false$' }).Count -eq 1)
  Assert "model_path 未配置 → 注释占位（与插件版同款）" (($y | Where-Object { $_ -match '^  # model_path:' }).Count -eq 1)
  Assert "旧键 min_code_len 不再产出" (($y | Where-Object { $_ -match 'min_code_len' }).Count -eq 0)
  Assert "旧键 max_code_len 不再产出" (($y | Where-Object { $_ -match 'max_code_len' }).Count -eq 0)
  Assert "旧键 min_tokens 不再产出" (($y | Where-Object { $_ -match 'min_tokens' }).Count -eq 0)
  Assert "旧键 com_context 不再产出" (($y | Where-Object { $_ -match 'com_context' }).Count -eq 0)
  Assert "llm_filter 组件仍恰 1 行" ((($y | Select-String 'llm_filter').Count) -eq 1)
  # 键序与插件版 Get-LlmCfgLines 逐字一致（两版配置节相同，仅组件行不同）
  $expect = @('  enabled: true', "  code_pattern: '[abcde]{4}'", '  max_tokens: 10',
              '  max_candidates: 5', '  cpu_cores: 4', '  freq_beta: 1.50',
              '  expected_length_weight: 0.20', '  debug_fusion: false')
  $i0 = 0; for ($i = 0; $i -lt $y.Count; $i++) { if ($y[$i] -match '^llm_rerank:') { $i0 = $i } }
  $got = @($y[($i0 + 1)..($i0 + 8)])
  Assert "节内键序与插件版一致" (($got -join '|') -eq ($expect -join '|'))
  Assert "不再产出全局 llm_rerank.yaml" (-not (Test-Path (Join-Path $sandbox "Rime\llm_rerank.yaml")))
  Write-Host "  --- 方案 llm_rerank 节 ---"
  $y[($i0)..([Math]::Min($i0 + 9, $y.Count - 1))] | ForEach-Object { Write-Host ("    " + $_) }

  # phase8: 旧全局 llm_rerank.yaml 的一次性迁移
  # （2026-09-30 起运行期不再读它；只有"方案节不存在"时 GUI 用它带出界面值）
  Write-Host "== phase8: 旧全局配置迁移（方案无节 → 界面带出 → 接入即写进方案）=="
  @'
schema:
  schema_id: zz_test
engine:
  filters:
    - simplifier
    - uniquifier
'@ | Out-File -FilePath $test -Encoding ascii
  @'
enabled: true
code_pattern: '.{3,4}'
expected_length_weight: 0.35
freq_beta: 2.25
max_tokens: 12
max_candidates: 6
cpu_cores: 7
debug_fusion: true
model_path: d:/gguf_models/legacy.gguf
'@ | Out-File -FilePath (Join-Path $sandbox "Rime\llm_rerank.yaml") -Encoding ascii
  # 刷新 = 重扫方案 + 重读配置节（此时无节 → 走迁移）
  [W]::SendMessage([W]::GetDlgItem($hw, 1032), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # IDC_SCHEMAREF
  Start-Sleep -Milliseconds 700
  $st = Read-Stat $hw
  Write-Host ("  status: " + $st)
  Assert "状态提示已带出旧全局配置" ($st -match '旧全局配置')
  $vCores = Read-Ctl $hw 1015; $vTok = Read-Ctl $hw 1014; $vBeta = Read-Ctl $hw 1022
  $vPat = Read-Ctl $hw 1016; $vModel = Read-Ctl $hw 1002; $vElw = Read-Ctl $hw 1021
  Write-Host ("  界面实测: cores='{0}' max_tokens='{1}' freq_beta='{2}' elw='{3}' code_pattern='{4}' model='{5}'" -f $vCores, $vTok, $vBeta, $vElw, $vPat, $vModel)
  Assert ("界面 cpu_cores = 7（迁移，实测 '$vCores'）") ($vCores -eq "7")
  Assert ("界面 max_tokens = 12（迁移，实测 '$vTok'）") ($vTok -eq "12")
  Assert ("界面 freq_beta = 2.25（迁移，实测 '$vBeta'）") ($vBeta -eq "2.25")
  Assert ("界面 code_pattern = .{3,4}（迁移，实测 '$vPat'）") ($vPat -eq ".{3,4}")
  Assert ("界面 model_path 尾部 legacy.gguf（迁移，实测 '$vModel'）") ($vModel -match 'legacy\.gguf$')
  $st = Click-Add $hw
  $f = Get-Content $test -Encoding UTF8
  Assert "迁移值写进方案节 cpu_cores: 7" (($f | Where-Object { $_ -match '^  cpu_cores: 7$' }).Count -eq 1)
  Assert "迁移值写进方案节 freq_beta: 2.25" (($f | Where-Object { $_ -match '^  freq_beta: 2\.25$' }).Count -eq 1)
  Assert "迁移值写进方案节 max_tokens: 12" (($f | Where-Object { $_ -match '^  max_tokens: 12$' }).Count -eq 1)
  Assert "迁移的 code_pattern 单引号落盘" (($f | Where-Object { $_ -match "^  code_pattern: '\.\{3,4\}'$" }).Count -eq 1)
  Assert "迁移的 model_path 落盘（正斜杠）" (($f | Where-Object { $_ -match '^  model_path: d:/gguf_models/legacy\.gguf$' }).Count -eq 1)
  Assert "debug_fusion: true 落盘" (($f | Where-Object { $_ -match '^  debug_fusion: true$' }).Count -eq 1)
}
finally {
  Get-Process WeaselLLMSetup -ErrorAction SilentlyContinue | Stop-Process -Force
  Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:fail -eq 0) { Write-Host "`nALL PASS" } else { Write-Host "`nFAILED: $($script:fail)"; exit 1 }
