# test_debug_viewer_pipe.ps1 — 调试窗口管道双节奏假服务端（2026-11-11 用户定案）
# 模拟打字测试必须同时覆盖两档节奏（真实时序按设计标准：按键间隔最小 50ms）：
#   fast: 每键 50ms、词间 50ms   —— 高频突发，测不丢行/不串行
#   slow: 每键 500ms、词间 2000ms —— >1s 空闲触发查看器读超时取消路径（v2026.11.11-2 修复点）
# 每词时序 = 3 个中间键（S|pattern 行）→ 第 4 码（L + C×3）→ 上屏（E 行）；
# slow 阶段词间插一条 R（编辑键重置）行。
# 用法：先启动本脚本（等待连接），再启动查看器（WeaselLLMDebug.exe 或插件版
#       debug\llm_debug_view.ps1）。本脚本为服务端（假 llm_filter 推送端）。
#   pwsh -File test_debug_viewer_pipe.ps1 [-Rhythm fast|slow|both] [-Words 3]
param(
  [ValidateSet('fast', 'slow', 'both')][string]$Rhythm = 'both',
  [int]$Words = 3
)
$ErrorActionPreference = 'Stop'

$p = [System.IO.Pipes.NamedPipeServerStream]::new(
  'rime_llm_live', [System.IO.Pipes.PipeDirection]::Out, 4)
$p.WaitForConnection()
$w = [System.IO.StreamWriter]::new($p, [System.Text.Encoding]::UTF8)
$w.AutoFlush = $true
$w.WriteLine('V|rime-llm-live|1|source|2026-11-11')
Write-Output ('viewer connected ' + (Get-Date -Format HH:mm:ss))

# 一词 = 3 个中间键(S) + 第 4 码(L + C×3) + 上屏(E)；$code4 词码、$words 候选词组
function Send-Word {
  param($n, [string]$phase, [int]$keyMs, [int]$wordMs,
        [string]$code4, [string[]]$cands, [string]$ctx, [int]$tick)
  $partials = @($code4.Substring(0, 1), $code4.Substring(0, 2), $code4.Substring(0, 3))
  foreach ($k in $partials) {
    $w.WriteLine("S|$(Get-Date -Format HH:mm:ss)|pattern|$k|-")
    Start-Sleep -Milliseconds $keyMs
  }
  # 协议 v2（2026-11-11）：L 尾加 prep；C 第 5/6 字段 = 衰减频(eff)/累计频(uni)
  # 原始值，尾接 CE 三分量 ce1/ce2/ce3（未乘权重）
  $t = Get-Date -Format HH:mm:ss
  $w.WriteLine("L|$t|$code4|-|tsf|$ctx|$tick|4$n|1|3|$($n % 2)|12345")
  # 数值一律 F3 格式化（与生产 %.3f 一致；PowerShell 浮点直接插值会输出
  # 17 位长串，查看器列宽会截断 —— 2026-11-11 实测踩过）
  for ($i = 0; $i -lt $cands.Count; $i++) {
    # c1 跨一位/两位整数（4.0/9.3/14.6）——验证查看器的动态右对齐；
    # 分量为原始交叉熵（正值），ce = -加权 Σ（与生产同语义，2026-11-11 修正）
    $c1 = 4.0 + $i * 5.3
    $c2 = 2.5 + $i * 0.1
    $c3 = 1.2 + $i * 0.05
    $weighted = -($c1 + 1.05 * $c2 + 0.45 * $c3)
    $eff = 0.5 + $i * 0.25
    $uni = 7 + $i
    # 词频得分 = 12·log(1+衰减计数)；累计词频 = log(累计次数+1)（2026-11-11
    # 公式简化：分母为窗内常数、排序不变量，已去除——与生产同语义）
    # fused = CE + 两项之和（查看器总分格显示连加过程，假数据须自洽）
    $fbS = 12.0 * [Math]::Log(1.0 + $eff)
    $u1S = [Math]::Log($uni + 1.0)
    $fusedS = $weighted + $fbS + $u1S
    $w.WriteLine(('C|{0}|{1}|{2:F3}|{3:F3}|{4:F3}|{5}|{6:F3}|0|1|1|1|{7:F3}|{8:F3}|{9:F3}|{10:F3}|{11:F3}' -f
      ($i + 1), $cands[$i], (1.0 + $i), $weighted, $eff, $uni,
      $fusedS, $c1, $c2, $c3, $fbS, $u1S))
  }
  $t2 = Get-Date -Format HH:mm:ss
  $w.WriteLine("E|$t2|$($cands[0])|0.5|1.0|$tick|$($tick + 1)")
  Write-Output ("word {0} ({1}) code={2} sent at {3}" -f $n, $phase, $code4, $t)
  Start-Sleep -Milliseconds $wordMs
}

if ($Rhythm -in 'fast', 'both') {
  Write-Output '== fast phase: 50ms/key, 50ms/word =='
  for ($i = 1; $i -le $Words; $i++) {
    Send-Word $i 'fast' 50 50 ("f" + $i * 1111) @("快词A$i", "快词B$i", "快词C$i") "快节奏第$i词的上文" (100 + $i)
  }
}
if ($Rhythm -in 'slow', 'both') {
  Write-Output '== slow phase: 500ms/key, 2000ms/word =='
  for ($i = 1; $i -le $Words; $i++) {
    if ($i -gt 1) {
      $w.WriteLine("R|$(Get-Date -Format HH:mm:ss)|编辑键(BackSpace) 慢速词间重置")
    }
    Send-Word $i 'slow' 500 2000 ("s" + $i * 1111) @("慢词A$i", "慢词B$i", "慢词C$i") "慢节奏第$i词的上文" (200 + $i)
  }
}
Start-Sleep -Seconds 2
$p.Dispose()
Write-Output 'server done'
