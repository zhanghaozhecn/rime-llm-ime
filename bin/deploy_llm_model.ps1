# deploy_llm_model.ps1 — 模型检查 + 可选下载
# 用法：powershell -ExecutionPolicy Bypass -File deploy_llm_model.ps1 [-ModelPath <路径>] [-Force]
# 默认路径已有模型 → 跳过；否则询问是否下载（约 500MB，ModelScope）。
# 下载用系统自带 curl.exe（-C - 断点续传），临时文件完成后改名（防半截文件被加载）。
# 不下载/下载失败 → 返回 0（LLM 不可用但输入法正常），脚本继续。

param(
  [string]$ModelPath = "d:\gguf_models\Qwen3.5-0.8B-Q4_K_M.gguf",
  [switch]$Force
)

$ErrorActionPreference = "Stop"
$URL = "https://modelscope.cn/models/unsloth/Qwen3.5-0.8B-GGUF/resolve/master/Qwen3.5-0.8B-Q4_K_M.gguf"

if (Test-Path $ModelPath) {
  $sz = (Get-Item $ModelPath).Length
  Write-Host ("  模型已存在（{0:N0} MB）：{1}" -f ($sz / 1MB), $ModelPath) -ForegroundColor Green
  exit 0
}

Write-Host "[警告] 模型未找到: $ModelPath" -ForegroundColor Yellow
Write-Host "  LLM 重排需要 Qwen3.5-0.8B-Q4_K_M.gguf（约 500MB）"
if (-not $Force) {
  $ans = Read-Host "  是否现在下载？[y/N]"
  if ($ans -notmatch '^[yY]') {
    Write-Host "  跳过下载 — LLM 重排不可用（输入法照常）。稍后可手动下载后重跑本脚本或 deploy_llm.bat" -ForegroundColor Yellow
    exit 0
  }
}

# 准备目录（D:\gguf_models 可能不存在）
$dir = Split-Path $ModelPath -Parent
if ($dir -and -not (Test-Path $dir)) {
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  Write-Host "  已创建目录: $dir"
}

$tmp = $ModelPath + ".download"
Write-Host "  下载中: $URL" -ForegroundColor Cyan
Write-Host "  （断点续传，可中断后重跑继续）"
curl.exe -L -C - --progress-bar -o $tmp $URL
if ($LASTEXITCODE -eq 0 -and (Test-Path $tmp)) {
  $sz = (Get-Item $tmp).Length
  if ($sz -gt 100MB) {
    Move-Item $tmp $ModelPath -Force
    Write-Host ("  下载完成: {0:N0} MB → {1}" -f ($sz / 1MB), $ModelPath) -ForegroundColor Green
    exit 0
  }
  Write-Host "  [错误] 下载文件异常（过小），已删除临时文件" -ForegroundColor Red
  Remove-Item $tmp -Force -ErrorAction SilentlyContinue
} else {
  Write-Host "  [错误] 下载失败（可重跑续传）。手动下载：" -ForegroundColor Red
  Write-Host "  $URL" -ForegroundColor Yellow
  Remove-Item $tmp -Force -ErrorAction SilentlyContinue
}
exit 0
