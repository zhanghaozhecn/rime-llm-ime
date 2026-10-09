# make_installer.ps1 — 二进制载荷同步（源码版仓库内开发工具）
# 仓库根 bin\ → installer\source\（11 个载荷文件；setup.iss 与安装包的载荷来源）。
# WinSparkle.dll = WeaselServer.exe 的静态导入依赖（从官方小狼毫安装目录拷入 bin\）；
#   2026-08-27 依赖审计发现：缺它全新机 WeaselServer 起不来（升级机被官方文件掩盖）。
# WeaselSetup.exe = 全新机 TSF 注册工具（scripts\build_setup.bat 产物拷入 bin\）。
# bench_threads.exe = 线程数实测工具（2026-10-01 用户定案）：规范源在**插件版仓库**
#   ..\rime-llm-rerank\bin\bench_threads.exe（源码 cpp\bench_threads.cpp 也只住那边），
#   本仓库不放副本（2026-09-30 去重铁律）→ 打包时现拷进 bin\ 与 installer\source\
#   （两处均已 gitignore，不入库）。WeaselLLMSetup.exe 按**同目录**找它。
# 同步后手动 commit 推送；安装包另行 scripts\build_pkg.bat 编译。
# 载荷变动后用 dumpbin /DEPENDENTS 逐个复核导入，防再出包外依赖。
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
# bench_threads.exe：从插件版仓库现拷（缺了直接失败——载荷少它会让 GUI 的
# 『实测线程数』按钮在真机上永远报"未找到"）
$benchSrc = Join-Path $repo "..\rime-llm-rerank\bin\bench_threads.exe"
if (-not (Test-Path $benchSrc)) {
  throw "bench_threads.exe not found: $benchSrc（先跑插件版 cpp\build_bench_threads.bat）"
}
Copy-Item $benchSrc (Join-Path $SourceBin "bench_threads.exe") -Force
Write-Host "+ bench_threads.exe ← 插件版 bin\（规范源）"
foreach ($f in @("rime.dll", "WeaselServer.exe", "WeaselDeployer.exe",
                 "weaselx64.dll", "weasel32.dll", "opencc.dll", "vcomp140.dll",
                 "WeaselLLMSetup.exe", "WeaselSetup.exe", "WinSparkle.dll",
                 "bench_threads.exe")) {
  $s = Join-Path $SourceBin $f
  if (Test-Path $s) { Copy-Item $s $dst -Force; Write-Host "+ $f" }
  else { Write-Host "MISS $f" -ForegroundColor Yellow }
}
Write-Host "同步完成 — git add installer && commit && push；安装包: scripts\build_pkg.bat"
