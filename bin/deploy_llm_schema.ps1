# deploy_llm_schema.ps1 — 源码版（rime-llm-ime）方案配置插入
# 用法：powershell -ExecutionPolicy Bypass -File deploy_llm_schema.ps1 [-SchemaName pdsp.schema.yaml]
# 在 RIME 用户目录的 <SchemaName> 中插入（幂等，可重复运行）：
#   1. engine.filters 的 uniquifier 之后插 - llm_filter（原生 C++ 组件，
#      先 LLM 重排、再固顶词提升——在 pin_fix_filter 之前）
#   2. 顶层 llm_rerank: 配置节（enabled: true + 参数）
# 完成后托盘重新部署生效。不修改方案源文件（操作对象 = 用户目录副本）。

param(
  [string]$SchemaName = "pdsp.schema.yaml",
  [switch]$Force
)

$ErrorActionPreference = "Stop"
$RIME = Join-Path $env:APPDATA "Rime"
$schema = Join-Path $RIME $SchemaName
if (-not (Test-Path $schema)) {
  Write-Host "[ERROR] 方案文件不存在: $schema" -ForegroundColor Red
  Write-Host "  请先把方案 yaml 复制到 RIME 用户目录，或用 -SchemaName 指定"
  exit 1
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$lines = [IO.File]::ReadAllLines($schema, [Text.Encoding]::UTF8)
$out = New-Object System.Collections.Generic.List[string]
$changed = $false

# 冲突检测：插件版（lua 版）组件存在 → 二选一，中止
if (($lines | Where-Object { $_ -match 'lua_filter@\*llm_filter' }).Count -gt 0) {
  Write-Host "[ERROR] 检测到插件版组件（lua_filter@*llm_filter）——源码版与插件版二选一，" -ForegroundColor Red
  Write-Host "  双重重排会导致行为混乱 + 双倍推理。请先从 schema 移除插件版行再运行。" -ForegroundColor Red
  exit 2
}

$hasFilt = ($lines | Where-Object { $_ -match '^\s+-\s+llm_filter(\s|$)' }).Count -gt 0
$hasCfg  = ($lines | Where-Object { $_ -match '^llm_rerank:' }).Count -gt 0

# 已存在时校验位置（uniquifier 之后、pin_fix 之前）——仅查存在性会漏掉
# "已有但位置错"（如手动加在末尾 → 固顶词被 LLM 顶掉）
if ($hasFilt) {
  $fBegin = -1; $fEnd = $lines.Count  # filters 块范围
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s+filters:') { $fBegin = $i; continue }
    if ($fBegin -ge 0 -and $lines[$i] -match '^\S' -and $i -gt $fBegin) { $fEnd = $i; break }
  }
  $pos = @{}  # 组件 → filters 块内行号（lua_filter@* 前缀的组件也匹配）
  for ($i = $fBegin; $i -lt $fEnd; $i++) {
    if ($lines[$i] -match '^\s+-\s+(?:lua_filter@\*)?(uniquifier|llm_filter|pin_fix_filter|hint_filter|no_match_placeholder)\s*$') {
      $pos[$matches[1]] = $i
    }
  }
  $ok = $true
  if ($pos.ContainsKey("uniquifier") -and $pos["llm_filter"] -lt $pos["uniquifier"]) {
    Write-Host "[警告] llm_filter 在 uniquifier 之前（去重顺序错误）——请调整或恢复默认方案后重跑" -ForegroundColor Yellow
    $ok = $false
  }
  if ($pos.ContainsKey("pin_fix_filter") -and $pos["llm_filter"] -gt $pos["pin_fix_filter"]) {
    Write-Host "[警告] llm_filter 在 pin_fix_filter 之后（固顶词会被 LLM 重排顶掉）——请调整或恢复默认方案后重跑" -ForegroundColor Yellow
    $ok = $false
  }
  if ($ok) { Write-Host "  llm_filter 已存在且位置正确（uniquifier 后、pin_fix 前），跳过" }
  elseif (-not $Force) {
    Write-Host "  中止（用 -Force 跳过位置检查）"; exit 3
  }
}

# 1. filters: uniquifier 行后插入 - llm_filter（精确位置）
if (-not $hasFilt) {
  $inFilt = $false; $inserted = $false
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s+filters:') { $out.Add($lines[$i]); $inFilt = $true; continue }
    if ($inFilt -and $lines[$i] -match '^\s+- uniquifier') {
      $out.Add($lines[$i])
      $out.Add("    - llm_filter")
      $inserted = $true; $inFilt = $false
      continue
    }
    $out.Add($lines[$i])
  }
  if ($inserted) { Write-Host "  + filters: - llm_filter（uniquifier 之后、pin_fix 之前）"; $changed = $true }
  else { Write-Host "  [警告] 未找到 filters 块或 uniquifier，跳过插入（请手动添加）" -ForegroundColor Yellow }
} else {
  foreach ($ln in $lines) { $out.Add($ln) }
}

# 2. 顶层 llm_rerank 配置节
if (-not $hasCfg) {
  $cfgLines = @(
    "",
    "llm_rerank:",
    "  enabled: true         # true=启用 LLM 重排 | false=关闭（组件透传，不推理）",
    "  min_code_len: 4       # 输入编码长度小于此值时不重排",
    "  # max_code_len: 0     # 编码长度上限（0=不限制）；超出不推理，与 min_code_len 组成触发区间",
    "  # long_word_first: false  # true=long-word-first: 按词长降序, 同词长按 CE 评分序",
    "  # freq_weight: 0.25  # 用户词频融合权重 (0=关闭); freq_k: 5 饱和常数",
    "  # freq_k: 5",
    "  # min_tokens: 1       # 上文 token 数小于此值时不推理（0 = 空上文也推理）",
    "  # max_tokens: 10      # 上文 token 上限，超出时从末尾截断",
    "  # max_candidates: 5   # 参与打分的候选数上限（其余按原序接尾）",
    "  # cpu_cores: 4        # 推理线程数（默认=GGML 默认；bench_threads.exe 实测后可自行调整）",
    "  # model_path: d:/gguf_models/Qwen3.5-0.8B-Q4_K_M.gguf  # GGUF 模型路径"
  )
  foreach ($cl in $cfgLines) { $out.Add($cl) }
  Write-Host "  + llm_rerank: 配置节（enabled: true）"; $changed = $true
}

if ($changed) {
  [IO.File]::WriteAllLines($schema, $out, $utf8NoBom)
  Write-Host "  schema 已更新（UTF-8 无 BOM，幂等：重复运行不重复插入）"
  Write-Host "  完成：托盘小狼毫 → 重新部署 → LLM 重排生效"
} else {
  Write-Host "  schema 无需修改（组件已存在）"
}
