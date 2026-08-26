# make_installer.ps1 — 二进制与共用脚本同步（源码版仓库内，2026-08-26 分仓版）
# 1) 仓库根 bin\ → installer\source\（7 个二进制；clone 本仓库即得安装载荷）
# 2) 平行插件版仓库的 installer\common.ps1 → 本目录（共用文件，两仓库保持一致）
# 同步后手动 commit 推送即发布（分发 = git clone 对应仓库，无 zip / Release）。
param(
  [string]$SourceBin = "",                          # 默认 <仓库根>\bin
  [string]$PluginInstaller = "D:\rime-llm-rerank\installer"
)
$ErrorActionPreference = "Stop"
$here = Split-Path $PSCommandPath -Parent
$repo = Split-Path $here -Parent
if (-not $SourceBin) { $SourceBin = Join-Path $repo "bin" }
if (-not (Test-Path (Join-Path $SourceBin "rime.dll"))) {
  throw "source binaries not found: $SourceBin (use -SourceBin)"
}
$dst = Join-Path $here "source"
New-Item -ItemType Directory -Path $dst -Force | Out-Null
foreach ($f in @("rime.dll", "WeaselServer.exe", "WeaselDeployer.exe",
                 "weaselx64.dll", "weasel32.dll", "opencc.dll", "vcomp140.dll")) {
  $s = Join-Path $SourceBin $f
  if (Test-Path $s) { Copy-Item $s $dst -Force; Write-Host "+ $f" }
  else { Write-Host "MISS $f" -ForegroundColor Yellow }
}
$commonSrc = Join-Path $PluginInstaller "common.ps1"
if (Test-Path $commonSrc) {
  Copy-Item $commonSrc (Join-Path $here "common.ps1") -Force
  Write-Host "+ common.ps1（自 $PluginInstaller 同步）"
} else {
  Write-Host "WARN 未找到平行插件版仓库（$commonSrc），common.ps1 未同步" -ForegroundColor Yellow
}
Write-Host "同步完成 — git add installer && commit && push 即发布"
