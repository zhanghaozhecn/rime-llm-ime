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
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageStr(IntPtr h, uint m, IntPtr w, string s);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageBuf(IntPtr h, uint m, IntPtr w, [Out][MarshalAs(UnmanagedType.LPWStr)] System.Text.StringBuilder s);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
  // 确认框查找（#32770 模态框属于 GUI 进程）+ 子控件枚举
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, ref int pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  public static List<IntPtr> Children(IntPtr p) {
    var found = new List<IntPtr>();
    EnumChildWindows(p, delegate(IntPtr h, IntPtr l) { found.Add(h); return true; }, IntPtr.Zero);
    return found;
  }
  public static IntPtr FindDialog(int pid) {
    IntPtr hit = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      int p = 0; GetWindowThreadProcessId(h, ref p);
      if (p != pid) return true;
      var sb = new System.Text.StringBuilder(64);
      GetClassName(h, sb, 64);
      if (sb.ToString() == "#32770") { hit = h; return false; }
      return true;
    }, IntPtr.Zero);
    return hit;
  }
}
"@

function Get-Hwnd {
  for ($t = 0; $t -lt 10; $t++) {
    # 同一时刻可能有多个 WeaselLLMSetup（上一次测试刚被强杀还没退干净 / 手工开着界面）——
    # `.MainWindowHandle` 在多进程时会**返回数组**，传进 P/Invoke 直接报
    # "cannot convert System.Object[] to System.IntPtr"（2026-10-01 实测踩到）。
    # 取 StartTime 最新的那个，避免这条假失败。
    $proc = Get-Process WeaselLLMSetup -ErrorAction SilentlyContinue |
            Sort-Object StartTime -Descending | Select-Object -First 1
    if ($proc -and $proc.MainWindowHandle) { return $proc.MainWindowHandle }
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
# 保存并生效（1101）= 自适应：缺节/缺组件行时自动补齐（原「接入 LLM」已并入，2026-10-01）
function Get-WText([IntPtr]$h) {
  # 按句柄取文本（确认框里的按钮文案要用它）
  if ($h -eq [IntPtr]::Zero) { return "" }
  $len = [int][W]::SendMessage($h, 0x000E, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($len -le 0) { return "" }
  $sb = New-Object System.Text.StringBuilder ($len + 2)
  [void][W]::SendMessageBuf($h, 0x000D, [IntPtr]($len + 1), $sb)
  return $sb.ToString()
}
# 点某个按钮（**必须 Post 不能 Send**：按钮处理里可能弹模态确认框，SendMessage 会把测试线程
# 与 GUI 线程一起卡在框上，后面的 Dismiss-Prompt 永远执行不到）
function Click-Ctl($hw, [int]$id, [string]$Dismiss = "") {
  [void][W]::PostMessage([W]::GetDlgItem($hw, $id), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($Dismiss) { [void](Dismiss-Prompt $hw $Dismiss) }
  Start-Sleep -Milliseconds 800
}
function Click-Save($hw) {
  [void][W]::PostMessage([W]::GetDlgItem($hw, 1101), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  [void](Dismiss-Prompt $hw "是")   # 若因"文件已被外部修改"弹框 → 答"是"（用界面值覆盖）
  Start-Sleep -Milliseconds 900
  return (Read-Stat $hw)
}
function Click-Add($hw) {
  [W]::SendMessage($combo, 0x014E, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # CB_SETCURSEL(0)
  Reload-Params $hw           # 先重读（测试刚在外部改过文件；否则触发"文件已变更"确认框）
  return (Click-Save $hw)     # 保存即自动接入（缺件补齐）
}
# 重读当前方案（刷新按钮已删）：向主窗口投 CBN_SELCHANGE —— 源码版直接读 CB_GETCURSEL，
# 故合成通知即可触发 load_schema_params（不依赖原生控件是否真的发了通知）。
# 界面有未保存改动时该动作会弹"未保存的改动"确认框（2026-10-01）→ 用 -Answer 自动应答。
function Reload-Params($hw, [string]$Answer = "是") {
  [void][W]::PostMessage($hw, 0x0111, [IntPtr](0x10000 * 1 + 1031), [IntPtr]$combo)   # WM_COMMAND(Post!), CBN_SELCHANGE
  if ($Answer) { [void](Dismiss-Prompt $hw $Answer) }
  Start-Sleep -Milliseconds 700
}
# 展开/收起方案下拉（触发 CBN_DROPDOWN(7)：重扫两处列表 + 界面无改动时重读当前文件）
function Open-Dropdown($hw) {
  [void][W]::PostMessage($hw, 0x0111, [IntPtr](0x10000 * 7 + 1031), [IntPtr]$combo)
  Start-Sleep -Milliseconds 800
}
# 找 GUI 进程（不是测试进程！）的模态确认框（#32770）并点掉指定按钮。
# 没弹框属正常（界面干净）→ 返回 $false，不抛异常，避免套件卡死在模态框上。
function Dismiss-Prompt([IntPtr]$hw, [string]$ButtonText) {
  $gpid = 0
  [void][W]::GetWindowThreadProcessId($hw, [ref]$gpid)
  $dlg = [IntPtr]::Zero
  for ($i = 0; $i -lt 25; $i++) {
    $dlg = [W]::FindDialog([int]$gpid)
    if ($dlg -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 200
  }
  if ($dlg -eq [IntPtr]::Zero) { return $false }
  foreach ($h in [W]::Children($dlg)) {
    if ((Get-WText $h) -eq $ButtonText) {
      [void][W]::SendMessage($h, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)   # BM_CLICK
      Start-Sleep -Milliseconds 400
      return $true
    }
  }
  # 按钮文案对不上（语言/ID 差异）→ 按 IDYES(6)/IDNO(7) 发 WM_COMMAND 兜底
  $id = if ($ButtonText -eq "是") { 6 } else { 7 }
  [void][W]::SendMessage($dlg, 0x0111, [IntPtr]$id, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 400
  return $true
}
function Click-Remove($hw) {
  [void][W]::PostMessage([W]::GetDlgItem($hw, 1034), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 900
  return (Read-Stat $hw)
}
# 方案下拉列表项（跨进程：CB_GETCOUNT + CB_GETLBTEXT）
function Get-ComboItems($hw) {
  $out = @()
  $n = [int][W]::SendMessage($hw, 0x0146, [IntPtr]::Zero, [IntPtr]::Zero)
  for ($i = 0; $i -lt $n; $i++) {
    $len = [int][W]::SendMessage($hw, 0x0149, [IntPtr]$i, [IntPtr]::Zero)
    if ($len -le 0) { continue }
    $sb = New-Object System.Text.StringBuilder ($len + 2)
    [void][W]::SendMessageBuf($hw, 0x0148, [IntPtr]$i, $sb)
    $out += $sb.ToString()
  }
  return $out
}
# 选最后一项（CB_SETCURSEL；原生 Win32 下拉框不响应直接投递的 VK_END）
function Select-ComboLast($hw) {
  $n = [int][W]::SendMessage($hw, 0x0146, [IntPtr]::Zero, [IntPtr]::Zero)   # CB_GETCOUNT
  if ($n -le 0) { return }
  [W]::SendMessage($hw, 0x014E, [IntPtr]($n - 1), [IntPtr]::Zero) | Out-Null  # CB_SETCURSEL
  Start-Sleep -Milliseconds 400
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
$env:RIME_LLM_USER_DIR = Join-Path $sandbox "Rime"   # 用户目录钉在沙箱（GUI 优先注册表 RimeUserDir）
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

  Write-Host "== phase1: 干净方案接入（保存并生效 = 自适应补齐）=="
  $st = Click-Add $hw
  Write-Host ("  status: " + $st)
  $f = Get-Content $test -Encoding UTF8
  $iU = 0; for ($i = 0; $i -lt $f.Count; $i++) { if ($f[$i] -match "uniquifier") { $iU = $i } }
  # 保存的自适应结果报在底部状态行（IDC_STATUS=1103）；方案接入组状态行变为"已加载"
  Assert "底部状态行含'已接入并保存'" ((Read-Ctl $hw 1103) -match "已接入并保存")
  Assert "方案接入组状态行变'已加载'" ((Read-Stat $hw) -match "已加载")
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
  [void][W]::PostMessage([W]::GetDlgItem($hw, 1101), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)  # IDC_SAVE
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
  # 重读配置节（刷新按钮已删 → 合成 CBN_SELCHANGE；此时无节 → 走旧全局配置迁移）
  Reload-Params $hw
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

  # phase9: 只在出错时写错误日志（GUI 程序目录 = bin\WeaselLLMSetup_error.log）
  Write-Host "== phase9: 出错写错误日志（正常不写）=="
  $logFile = Join-Path (Split-Path $exe -Parent) "WeaselLLMSetup_error.log"
  if (Test-Path $logFile) { Remove-Item $logFile -Force }
  [void][W]::PostMessage([W]::GetDlgItem($hw, 1101), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)  # 正常保存
  Start-Sleep -Milliseconds 700
  Assert "正常保存不写错误日志" (-not (Test-Path $logFile))
  Set-ItemProperty -Path $test -Name IsReadOnly -Value $true          # 方案只读 → 保存必失败
  [void][W]::PostMessage([W]::GetDlgItem($hw, 1101), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 700
  Assert "状态行出现失败" ((Read-Ctl $hw 1103) -match '失败')
  Assert "错误日志已生成" (Test-Path $logFile)
  if (Test-Path $logFile) {
    $lg = Get-Content $logFile -Raw -Encoding UTF8
    Assert "日志含『写入方案失败』" ($lg -match '写入方案失败')
    Write-Host ("  日志首行: " + ($lg -split "`r?`n")[0])
  }
  Set-ItemProperty -Path $test -Name IsReadOnly -Value $false

  # phase10: 程序文件夹预装方案可选中 + 写入前自动复制到用户文件夹
  Write-Host "== phase10: 程序文件夹预装方案可选中 + 保存即自动复制到用户文件夹并接入 =="
  $items = Get-ComboItems $combo
  Write-Host ("  方案下拉（{0} 项）: {1}" -f $items.Count, ($items -join ' | '))
  $prog = @($items | Where-Object { $_ -like "*（程序）" })
  Assert ("程序文件夹预装方案已列出（实测 $($prog.Count) 个）") ($prog.Count -ge 1)
  $progName = @($prog | Select-Object -Last 1) -replace '（程序）$', ''
  $root = (Get-ItemProperty 'HKLM:\SOFTWARE\Rime\Weasel' -Name WeaselRoot).WeaselRoot
  $sharedFile = Join-Path (Join-Path $root 'data') $progName
  $beforeHash = (Get-FileHash $sharedFile -Algorithm SHA256).Hash
  Select-ComboLast $combo
  Reload-Params $hw
  Assert ("已选中预装方案（实测 '$(Read-Ctl $hw 1031)'）") ((Read-Ctl $hw 1031) -like "*（程序）")
  Assert "选中预装方案时状态提示会先复制" ((Read-Stat $hw) -match '预装方案')
  # 注意：不能走 Click-Add（它内部会把选择重置到第 0 项）；这里直接点「保存并生效」（= 复制 + 自动接入）
  [void][W]::PostMessage([W]::GetDlgItem($hw, 1101), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 1200
  $st = Read-Stat $hw
  Write-Host ("  status: " + $st)
  $copied = Join-Path (Join-Path $sandbox "Rime") $progName
  Assert "预装方案已复制到用户文件夹" (Test-Path $copied)
  if (Test-Path $copied) {
    $cf = Get-Content $copied -Encoding UTF8
    Assert "复制件含 llm_filter 组件行" (($cf | Where-Object { $_ -match '^\s+- llm_filter\s*$' }).Count -eq 1)
    Assert "复制件含 llm_rerank 节" (($cf | Where-Object { $_ -match '^llm_rerank:' }).Count -eq 1)
    Assert "复制件 enabled: true" (($cf | Where-Object { $_ -match '^\s+enabled: true\s*$' }).Count -eq 1)
    Remove-Item $copied -Force
  }
  Assert "程序文件夹原件未被改动" ((Get-FileHash $sharedFile -Algorithm SHA256).Hash -eq $beforeHash)

  # phase11: 保存的自适应重建（缺组件行也自动补齐）+ 打开下拉重扫列表 / 脏标记保护
  Write-Host "== phase11: 保存自动补组件行 + 打开下拉重扫 + 脏标记保护 =="
  # 先重扫列表（phase10 复制又删掉的预装方案副本会残留成死项），再按名选中沙箱方案
  Open-Dropdown $hw
  $idx = [int][W]::SendMessageStr($combo, 0x0158, [IntPtr]::Zero, "zz_test_gui.schema.yaml")
  Assert ("按名找到沙箱方案（CB_FINDSTRINGEXACT=$idx）") ($idx -ge 0)
  [W]::SendMessage($combo, 0x014E, [IntPtr]$idx, [IntPtr]::Zero) | Out-Null   # CB_SETCURSEL
  Reload-Params $hw
  Write-Host ("  选中方案: '" + (Read-Ctl $hw 1031) + "' | 方案接入组: " + (Read-Stat $hw))
  Assert ("选中方案 = zz_test_gui（实测 '" + (Read-Ctl $hw 1031) + "'）") ((Read-Ctl $hw 1031) -eq "zz_test_gui.schema.yaml")
  # (a) 节在、组件行被删 → 保存应自动补回 llm_filter
  $txt = Get-Content $test -Raw -Encoding UTF8
  $txt = ($txt -split "`r?`n" | Where-Object { $_ -notmatch '^\s+- llm_filter\s*$' }) -join "`r`n"
  Set-Content $test -Value $txt -Encoding ascii -NoNewline
  Reload-Params $hw
  $st = Click-Save $hw
  Write-Host ("  status: " + $st + " | 底部: " + (Read-Ctl $hw 1103))
  $f11 = Get-Content $test -Encoding UTF8
  Assert "缺组件行时保存自动补齐（底部状态含 已接入并保存）" ((Read-Ctl $hw 1103) -match '已接入并保存')
  Assert "llm_filter 组件行已补回" (($f11 | Where-Object { $_ -match '^\s+- llm_filter\s*$' }).Count -eq 1)
  Assert "配置节未重复" (($f11 | Where-Object { $_ -match '^llm_rerank:' }).Count -eq 1)
  # (b) 打开下拉 → 重扫列表（外部新增方案出现）
  $extra = Join-Path (Split-Path $test -Parent) "zz_extra.schema.yaml"
  "schema:`r`n  schema_id: zz_extra`r`nengine:`r`n  filters:`r`n    - uniquifier`r`n" |
    Out-File -FilePath $extra -Encoding ascii
  Open-Dropdown $hw
  $items = Get-ComboItems $combo
  Write-Host ("  方案下拉（{0} 项）: {1}" -f $items.Count, ($items -join ' | '))
  Assert "打开下拉后新方案已出现在列表" (@($items | Where-Object { $_ -eq 'zz_extra.schema.yaml' }).Count -eq 1)
  Remove-Item $extra -Force
  # (c) 脏标记：界面有未保存改动时，打开下拉不覆盖用户输入
  $tok = [W]::GetDlgItem($hw, 1014)     # IDC_MAX_TOK
  [void][W]::SendMessageStr($tok, 0x000C, [IntPtr]::Zero, "77")   # WM_SETTEXT
  $raw = Get-Content $test -Raw -Encoding UTF8
  Set-Content $test -Value ($raw -replace 'max_tokens: \d+', 'max_tokens: 55') -Encoding ascii -NoNewline
  Open-Dropdown $hw
  Assert ("未保存的界面输入未被冲掉（实测 '$(Read-Ctl $hw 1014)'）") ((Read-Ctl $hw 1014) -eq "77")
  # (d) 切换方案时若有未保存改动：选"否"→ 保留改动、不重读；选"是"→ 丢弃并重读
  Reload-Params $hw "否"
  Assert ("切换被取消后界面值保留（实测 '$(Read-Ctl $hw 1014)'）") ((Read-Ctl $hw 1014) -eq "77")
  Assert "取消提示出现在状态行" ((Read-Ctl $hw 1103) -match '已取消切换')
  Reload-Params $hw "是"
  $diag = "选中='" + (Read-Ctl $hw 1031) + "' 文件 max_tokens 行='" + (((Get-Content $test) | Where-Object { $_ -match 'max_tokens:' }) -join '/') + "'"
  Write-Host ("  [诊断] " + $diag)
  Assert ("确认切换后取磁盘值（实测 '$(Read-Ctl $hw 1014)'；$diag）") ((Read-Ctl $hw 1014) -eq "55")

  # phase12: 五处薄弱点修复的覆盖（模型路径绝对化 / 可疑文件 / 下载前确认 / 遗留 curl 接管）
  Write-Host "== phase12: 模型路径与下载的新校验 =="
  # ⑤ 默认路径跟着"用户目录"（沙箱由 RIME_LLM_USER_DIR 指定）
  # 两个干扰源都要先清掉：① 旧全局 llm_rerank.yaml 的迁移值 ② 方案节里 phase8 写进去的显式 model_path
  Remove-Item (Join-Path $sandbox "Rime\llm_rerank.yaml") -Force -ErrorAction SilentlyContinue
  $raw5 = Get-Content $test -Raw -Encoding UTF8
  $raw5 = ($raw5 -split "`r?`n" | Where-Object { $_ -notmatch '^\s+#?\s*model_path:' }) -join "`r`n"
  Set-Content $test -Value $raw5 -Encoding ascii -NoNewline
  Reload-Params $hw
  $modelCtl = [W]::GetDlgItem($hw, 1002)
  $cur = Read-Ctl $hw 1002
  $expectDefault = Join-Path (Join-Path $sandbox "Rime") "Qwen3.5-0.8B-Q4_K_M.gguf"
  Write-Host ("  模型路径框 = " + $cur + "（方案节已无 model_path + 无旧全局 yaml）")
  Assert ("默认模型路径 = 用户目录下的默认名（实测 '$cur'）") ($cur -eq $expectDefault)
  # ① 指向一个明显偏小的 .gguf → 状态行报"可疑"（不再谎报已就绪）
  $small = Join-Path (Join-Path $sandbox "Rime") "small.gguf"
  [IO.File]::WriteAllBytes($small, (New-Object byte[] 2048))
  [void][W]::SendMessageStr($modelCtl, 0x000C, [IntPtr]::Zero, $small)
  # 跨进程 WM_SETTEXT 不会给 GUI 发编辑通知 → 手动投 CBN_EDITUPDATE，触发状态行刷新
  [void][W]::PostMessage($hw, 0x0111, [IntPtr](0x10000 * 6 + 1002), $modelCtl)   # CBN_EDITUPDATE=6
  Start-Sleep -Milliseconds 500
  Assert ("小文件显示为可疑（实测 '$(Read-Ctl $hw 1005)'）") ((Read-Ctl $hw 1005) -match '可疑')
  # ①b 已存在但偏小时点下载 → 先弹确认；选"否"→ 取消且不启动下载
  $curlBefore = @(Get-Process curl -ErrorAction SilentlyContinue).Count
  [void][W]::PostMessage([W]::GetDlgItem($hw, 1007), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  Dismiss-Prompt $hw "否" | Out-Null
  Start-Sleep -Milliseconds 400
  Assert "取消下载提示出现在状态行" ((Read-Ctl $hw 1103) -match '已取消下载')
  Assert ("未启动新的 curl（实测 {0} 个）" -f @(Get-Process curl -ErrorAction SilentlyContinue).Count) (@(Get-Process curl -ErrorAction SilentlyContinue).Count -eq $curlBefore)
  Remove-Item $small -Force
  # ③ 相对路径必须被拒绝（不落盘）
  [void][W]::SendMessageStr($modelCtl, 0x000C, [IntPtr]::Zero, "model.gguf")
  Start-Sleep -Milliseconds 300
  $before12 = (Get-FileHash $test -Algorithm SHA1).Hash
  [void][W]::PostMessage([W]::GetDlgItem($hw, 1101), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 700
  Assert ("相对路径保存被拒绝（实测 '" + (Read-Ctl $hw 1103) + "'）") ((Read-Ctl $hw 1103) -match '必须是绝对路径')
  Assert "被拒绝时方案文件未被改写" ((Get-FileHash $test -Algorithm SHA1).Hash -eq $before12)
  # 恢复成默认路径（界面干净化 → 后续动作不再弹确认）
  [void][W]::SendMessageStr($modelCtl, 0x000C, [IntPtr]::Zero, $expectDefault)
  Start-Sleep -Milliseconds 300
  Click-Save $hw | Out-Null
  Assert ("恢复默认路径后保存成功（实测 '" + (Read-Ctl $hw 1103) + "'）") ((Read-Ctl $hw 1103) -match '已保存|已接入并保存')

  # ② 遗留 curl 接管：起一次下载 → 强杀 GUI（跳过关窗清理）→ 重开再点下载 → 旧进程被接管
  if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "  （跳过：本机无 curl.exe）"
  } else {
    $dlTarget = Join-Path (Join-Path $sandbox "Rime") "dl_test.gguf"
    $procA = Start-Process -FilePath $exe -PassThru
    $hwA = [IntPtr]::Zero
    for ($t = 0; $t -lt 30; $t++) { Start-Sleep -Milliseconds 500; $procA.Refresh(); if ($procA.HasExited) { break }; if ($procA.MainWindowHandle -ne [IntPtr]::Zero) { $hwA = $procA.MainWindowHandle; break } }
    if ($hwA -eq [IntPtr]::Zero) { throw "第二次启动 GUI 失败" }
    Start-Sleep -Seconds 1
    [void][W]::SendMessageStr([W]::GetDlgItem($hwA, 1002), 0x000C, [IntPtr]::Zero, $dlTarget)
    Start-Sleep -Milliseconds 300
    [void][W]::PostMessage([W]::GetDlgItem($hwA, 1007), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Seconds 3
    $tmpA = $dlTarget + ".download"
    Assert ("下载已启动：分片出现（{0} 字节）" -f $(if (Test-Path $tmpA) { (Get-Item $tmpA).Length } else { -1 })) (Test-Path $tmpA)
    Assert "下载已启动：pid 文件出现" (Test-Path ($tmpA + ".pid"))
    # 强杀 GUI（TerminateProcess 不经 WM_DESTROY）→ 模拟"关窗没清干净"的遗留 curl
    [void](Stop-Process -Id $procA.Id -Force)
    Start-Sleep -Seconds 1
    $orphanPid = (Get-Content ($tmpA + ".pid") -ErrorAction SilentlyContinue | Select-Object -First 1)
    $nOrphan = @(Get-Process curl -ErrorAction SilentlyContinue).Count
    Write-Host ("  [诊断] 遗留 curl PID=$orphanPid（curl 进程数 $nOrphan）")
    Assert ("强杀 GUI 后 curl 成为遗留进程（{0} 个）" -f $nOrphan) ($nOrphan -ge 1)
    # 重开 GUI → 再点下载 → 旧的应被结束、本次接管
    $procB = Start-Process -FilePath $exe -PassThru
    $hwB = [IntPtr]::Zero
    for ($t = 0; $t -lt 30; $t++) { Start-Sleep -Milliseconds 500; $procB.Refresh(); if ($procB.HasExited) { break }; if ($procB.MainWindowHandle -ne [IntPtr]::Zero) { $hwB = $procB.MainWindowHandle; break } }
    Start-Sleep -Seconds 1
    [void][W]::SendMessageStr([W]::GetDlgItem($hwB, 1002), 0x000C, [IntPtr]::Zero, $dlTarget)
    Start-Sleep -Milliseconds 300
    [void][W]::PostMessage([W]::GetDlgItem($hwB, 1007), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Seconds 2
    # 状态行会被进度行覆盖，故按 .pid 换人 + 旧进程消失来判定"接管"（比抓提示文本可靠）
    $newPid = (Get-Content ($tmpA + ".pid") -ErrorAction SilentlyContinue | Select-Object -First 1)
    Write-Host ("  [诊断] 接管后 pid 文件 = $newPid（旧 $orphanPid）")
    Assert ("接管后 pid 文件已换成新进程（旧 $orphanPid → 新 $newPid）") ($newPid -and ("$newPid" -ne "$orphanPid"))
    Assert ("旧下载进程已被结束（PID $orphanPid）") (-not (Get-Process -Id ([int]$orphanPid) -ErrorAction SilentlyContinue))
    Assert ("接管后只剩本次一个 curl（实测 {0} 个）" -f @(Get-Process curl -ErrorAction SilentlyContinue).Count) (@(Get-Process curl -ErrorAction SilentlyContinue).Count -eq 1)
    Get-Process curl -ErrorAction SilentlyContinue | Stop-Process -Force
    [void](Stop-Process -Id $procB.Id -Force)
    Remove-Item ($dlTarget + "*") -Force -ErrorAction SilentlyContinue
  }
}
finally {
  Get-Process WeaselLLMSetup -ErrorAction SilentlyContinue | Stop-Process -Force
  Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:fail -eq 0) { Write-Host "`nALL PASS" } else { Write-Host "`nFAILED: $($script:fail)"; exit 1 }
