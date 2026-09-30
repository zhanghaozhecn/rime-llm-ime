# schema_add.ps1 - 源码版（rime-llm-ime）方案接入助手
#
# 作用：向选中的 RIME 方案幂等插入 / 剥离 llm_filter 组件行 + llm_rerank 配置节。
#   - 插入位置：filters 块内 uniquifier 之后（无 uniquifier 则 simplifier
#     之后，再否则 filters 块末尾）——重排拿到的是其上方 filter 处理过的候选
#   - 先剥后插（2026-08-26 同款语义）：无论原状是无 LLM / 源码版 / 插件版
#     组件，先统一剥净再全新插入——跨版切换自动收敛，无需恢复原始方案配置
#   - 插件版组件行（lua_processor@*llm_processor / lua_filter@*llm_filter）
#     与 llm_rerank 配置节一并剥离后再插源码版组件
#   - **配置节也由本脚本写入**（2026-09-30 定案：配置回归方案，取消全局
#     %APPDATA%\Rime\llm_rerank.yaml）：模板与插件版逐字一致，两版方案配置
#     节相同、仅组件行不同；参数后续用托盘"LLM 重排设置"改（同样写这一节）
#   - 幂等：重复运行不重复插入
#
# 用法：
#   右键"使用 PowerShell 运行"            交互模式（列方案、选序号）
#   powershell -ExecutionPolicy Bypass -File schema_add.ps1
#   powershell -ExecutionPolicy Bypass -File schema_add.ps1 -SchemaName pdsp.schema.yaml -Action add
#   powershell -ExecutionPolicy Bypass -File schema_add.ps1 -SchemaName pdsp.schema.yaml -Action remove
#   -ModelPath D:\gguf_models\x.gguf     写入配置节 model_path（可选）
#
# 完成后脚本自动触发一次重新部署（WeaselDeployer /deploy），失败则请手动：
# 托盘小狼毫 → 重新部署。

param(
  [string]$SchemaName = "",
  [ValidateSet("add", "remove")]
  [string]$Action = "add",
  [string]$UserDir = "$env:APPDATA\Rime",
  [string]$ModelPath = ""
)

$ErrorActionPreference = "Stop"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Read-Schema([string]$path) { [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8) }
function Write-Schema([string]$path, $lines) { [IO.File]::WriteAllLines($path, $lines, $Utf8NoBom) }

# 剥离 LLM 组件：两版组件行 + llm_rerank 整节（含前置空行）
function Edit-SchemaRemove([string]$schemaPath) {
  $lines = Read-Schema $schemaPath
  $out = New-Object System.Collections.Generic.List[string]
  $inCfg = $false; $removed = 0
  foreach ($ln in $lines) {
    if ($inCfg) {
      if ($ln -match '^\S') { $inCfg = $false } else { $removed++; continue }
    }
    if ($ln -match '^\s*-\s+lua_processor@\*llm_processor\s*$' -or
        $ln -match '^\s*-\s+lua_filter@\*llm_filter\s*$' -or
        $ln -match '^\s*-\s+llm_filter\s*$') { $removed++; continue }
    if ($ln -match '^llm_rerank:') {
      $removed++
      if ($out.Count -gt 0 -and $out[$out.Count - 1] -match '^\s*$') {
        [void]$out.RemoveAt($out.Count - 1); $removed++
      }
      $inCfg = $true; continue
    }
    [void]$out.Add($ln)
  }
  if ($removed -eq 0) { Write-Host "  未发现 LLM 组件，文件未改动"; return $false }
  Write-Schema $schemaPath $out
  Write-Host ("  已移除 LLM 组件（含配置节）共 " + $removed + " 行")
  return $true
}

# 源码版组件：filters 块内 uniquifier 之后插 - llm_filter
# （回退：simplifier 之后 → filters 块末尾；无 filters 块则报错）
function Edit-SchemaSource([string]$schemaPath) {
  $lines = Read-Schema $schemaPath
  if (($lines | Where-Object { $_ -match '^\s*-\s+llm_filter\s*$' }).Count -gt 0) {
    Write-Host "  llm_filter 组件已存在，无需修改（幂等）"
    return $false
  }
  $out = New-Object System.Collections.Generic.List[string]
  foreach ($l in $lines) { [void]$out.Add($l) }
  $inFilt = $false; $filtStart = -1; $uniquifier = -1; $simplifier = -1; $filtEnd = -1
  for ($i = 0; $i -lt $out.Count; $i++) {
    if ($out[$i] -match '^\s+filters:') { $inFilt = $true; $filtStart = $i; continue }
    if ($inFilt) {
      if ($out[$i] -match '^\S') { $filtEnd = $i - 1; break }
      if ($out[$i] -match '^\s+-\s+uniquifier') { $uniquifier = $i }
      if ($simplifier -lt 0 -and $out[$i] -match '^\s+-\s+simplifier') { $simplifier = $i }
      $filtEnd = $i
    }
  }
  if ($filtStart -lt 0 -or $filtEnd -lt $filtStart) { throw "未找到 filters 块，无法插入组件" }
  $at = $uniquifier
  if ($at -lt 0) { $at = $simplifier }
  if ($at -ge 0) {
    $out.Insert($at + 1, "    - llm_filter")
    Write-Host ("  + filters: llm_filter（" + $(if ($uniquifier -ge 0) { "uniquifier" } else { "simplifier" }) + " 之后）")
  } else {
    $out.Insert($filtEnd + 1, "    - llm_filter")
    Write-Host "  + filters: llm_filter（filters 块末尾）"
  }
  Write-Schema $schemaPath $out
  Write-Host "  schema 已更新（幂等，重复运行不重复插入）"
  return $true
}

# llm_rerank 配置节模板 —— 与插件版 rime-llm-rerank\installer\install_plugin.ps1
# 的 Get-LlmCfgLines **逐字一致**（2026-09-30 用户定案：两版方案配置节相同，
# 仅组件行不同）：enabled / code_pattern / max_tokens / max_candidates /
# cpu_cores / freq_beta / expected_length_weight / debug_fusion / model_path
function Get-LlmCfgLines([string]$modelPath) {
  $l = @(
    "", "llm_rerank:",
    "  enabled: true",
    "  code_pattern: '.{4}'",
    "  max_tokens: 10",
    "  max_candidates: 5",
    "  cpu_cores: 4",
    "  freq_beta: 1.50",
    "  expected_length_weight: 0.20",
    "  debug_fusion: false"
  )
  if ($modelPath) { $l += "  model_path: " + ($modelPath -replace '\\', '/') }
  else { $l += "  # model_path: <绝对路径；默认 = $UserDir\Qwen3.5-0.8B-Q4_K_M.gguf>" }
  return $l
}

# 追加 llm_rerank 配置节（方案里已有则不动 = 幂等）；配置回归方案（2026-09-30
# 取消全局 %APPDATA%\Rime\llm_rerank.yaml，GUI 也写这一节）
function Add-LlmSection([string]$schemaPath, [string]$modelPath) {
  $lines = Read-Schema $schemaPath
  if (($lines | Where-Object { $_ -match '^llm_rerank:' }).Count -gt 0) {
    Write-Host "  llm_rerank 配置节已存在，未改动（幂等）"
    return $false
  }
  $out = New-Object System.Collections.Generic.List[string]
  foreach ($l in $lines) { [void]$out.Add($l) }
  Get-LlmCfgLines $modelPath | ForEach-Object { [void]$out.Add($_) }
  Write-Schema $schemaPath $out
  Write-Host "  + llm_rerank: 配置节（enabled: true，键序与插件版相同）"
  return $true
}

# 触发重新部署：部署器在安装目录（注册表 WeaselRoot 指向，setup.iss 幂等写入）
function Invoke-Redeploy {
  if ($env:SCHEMA_ADD_NO_REDEPLOY -eq "1") { Write-Host "  （SCHEMA_ADD_NO_REDEPLOY=1：跳过重新部署）"; return }
  $deployer = $null
  foreach ($key in @("HKLM:\SOFTWARE\Rime\Weasel", "HKLM:\SOFTWARE\WOW6432Node\Rime\Weasel")) {
    try {
      $root = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).WeaselRoot
      if ($root -and (Test-Path (Join-Path $root "WeaselDeployer.exe"))) {
        $deployer = Join-Path $root "WeaselDeployer.exe"; break
      }
    } catch { }
  }
  if (-not $deployer) {
    Write-Host "  [提示] 未定位到 WeaselDeployer.exe，请手动：托盘小狼毫 → 重新部署"
    return
  }
  Write-Host "  触发重新部署（WeaselDeployer /deploy）…"
  try {
    $p = Start-Process -FilePath $deployer -ArgumentList "/deploy" -PassThru
    if ($p.WaitForExit(15000)) { Write-Host ("  重新部署退出码: " + $p.ExitCode) }
    else { Write-Host "  重新部署仍在后台进行；若候选异常请托盘手动重新部署" }
  } catch { Write-Host "  [提示] 自动重新部署失败，请手动：托盘小狼毫 → 重新部署" }
}

function Get-SchemaPath([string]$name) {
  $p = Join-Path $UserDir $name
  if (-not (Test-Path $p)) { throw "方案文件不存在: $p（请先把方案 yaml 放入 $UserDir）" }
  return $p
}

# ── 交互模式（无 -SchemaName 时）──────────────────
if (-not $SchemaName) {
  if (-not (Test-Path $UserDir)) { throw "RIME 用户目录不存在: $UserDir" }
  $files = @(Get-ChildItem -Path $UserDir -Filter *.schema.yaml | Sort-Object Name)
  if ($files.Count -eq 0) { throw "$UserDir 下未找到 *.schema.yaml" }
  Write-Host ""
  Write-Host "=== 源码版方案接入：向方案插入 llm_filter 组件（幂等）==="
  Write-Host ("RIME 用户目录: " + $UserDir)
  Write-Host ""
  for ($i = 0; $i -lt $files.Count; $i++) { Write-Host ("  " + ($i + 1).ToString() + ". " + $files[$i].Name) }
  Write-Host ""
  $sel = Read-Host "选择方案序号（回车 = 1）"
  if ($sel -eq "") { $sel = "1" }
  $n = 0
  if (-not [int]::TryParse($sel, [ref]$n) -or $n -lt 1 -or $n -gt $files.Count) { throw "无效序号: $sel" }
  $SchemaName = $files[$n - 1].Name
  $a = Read-Host "动作 add=加入 / remove=移除（回车 = add）"
  if ($a -ne "" -and $a -ne "add" -and $a -ne "remove") { throw "无效动作: $a（只支持 add/remove）" }
  if ($a -ne "") { $Action = $a }
}

$schemaPath = Get-SchemaPath $SchemaName
Write-Host ""
Write-Host ("── 方案: " + $schemaPath)
if ($Action -eq "add") {
  # 先剥后插：原状无论无 LLM / 源码版 / 插件版均先剥净再插入（跨版自动转换）
  [void](Edit-SchemaRemove $schemaPath)
  [void](Edit-SchemaSource $schemaPath)
  [void](Add-LlmSection $schemaPath $ModelPath)
} else {
  [void](Edit-SchemaRemove $schemaPath)
}
Invoke-Redeploy
Write-Host ""
Write-Host "完成。参数配置：托盘右键 → LLM 重排设置（写方案 llm_rerank 节，保存即重新部署生效）"

# 交互运行时停一下，避免窗口直接消失
if ($Host.Name -eq "ConsoleHost" -and [Environment]::UserInteractive -and -not $PSBoundParameters.ContainsKey("SchemaName")) {
  [void](Read-Host "回车退出")
}
