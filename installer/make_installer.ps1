# make_installer.ps1 — 二进制载荷同步（源码版仓库内开发工具）
# 仓库根 bin\ → installer\source\（9 个二进制；setup.iss 与安装包的载荷来源）。
# 同步后手动 commit 推送；安装包另行 scripts\build_pkg.bat 编译。
# （2026-08-27 用户定案：源码版安装唯一入口 = setup.exe 安装包；
#   旧 install_source PowerShell 安装器已退役，common.ps1 跨仓同步随之取消）
param(
  [string]$SourceBin = ""                           # 默认 <仓库根>\bin
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
                 "weaselx64.dll", "weasel32.dll", "opencc.dll", "vcomp140.dll",
                 "WeaselLLMSetup.exe")) {
  $s = Join-Path $SourceBin $f
  if (Test-Path $s) { Copy-Item $s $dst -Force; Write-Host "+ $f" }
  else { Write-Host "MISS $f" -ForegroundColor Yellow }
}
Write-Host "同步完成 — git add installer && commit && push；安装包: scripts\build_pkg.bat"
