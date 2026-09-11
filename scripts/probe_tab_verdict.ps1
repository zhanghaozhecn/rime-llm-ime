# probe_tab_verdict.ps1 - replicates the -4 DLL decision pipeline per sample
# (no typing needed): writer wta (root==fg && visible, fresh-attach semantics)
# -> if YES: COM writer (report doc); else KWPP + presentation-name-in-title
# check -> COM ppt; else no COM (falls to TSF/UIA/history).
$log = "D:\tab_verdict.log"
Remove-Item $log -ErrorAction SilentlyContinue
Add-Type -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint ga);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
'@ -Name U32 -Namespace WP3
function Get-Tab {
  param([string]$cls)
  return ($cls -eq "OpusApp" -or $cls -eq "PP12FrameClass")
}
$end = (Get-Date).AddSeconds(150)
while ((Get-Date) -lt $end) {
  $line = Get-Date -Format "HH:mm:ss.fff"
  $fg = [WP3.U32]::GetForegroundWindow()
  $sb = New-Object System.Text.StringBuilder 256
  [void][WP3.U32]::GetClassName($fg, $sb, 256)
  $cls = $sb.ToString()
  $ttlB = New-Object System.Text.StringBuilder 256
  [void][WP3.U32]::GetWindowText($fg, $ttlB, 256)
  $ttl = $ttlB.ToString()
  if (-not (Get-Tab $cls)) {
    $line = "$line verdict=SKIP(not office fg) FG: $cls [$ttl]"
    Add-Content -Path $log -Value $line -Encoding UTF8
    Start-Sleep -Milliseconds 500
    continue
  }
  # --- writer pipeline (fresh GetActiveObject each sample == re-attach semantics)
  $wta = "UNKNOWN"
  $wdoc = ""
  try {
    $app = [Runtime.InteropServices.Marshal]::GetActiveObject('Kwps.Application')
    try { $wdoc = $app.ActiveWindow.Document.Name } catch {}
    try {
      $h = [IntPtr]$app.ActiveWindow.Hwnd
      $root = [WP3.U32]::GetAncestor($h, 2)
      if ($root -ne [IntPtr]::Zero -and $root -eq $fg) {
        if ([WP3.U32]::IsWindowVisible($h)) { $wta = "YES" } else { $wta = "NO" }
      } else { $wta = "NO" }
    } catch { $wta = "UNKNOWN" }
  } catch { $wta = "UNKNOWN" }
  if ($wta -eq "YES") {
    $line = "$line verdict=COM->writer[$wdoc]"
  } else {
    # --- ppt pipeline: name-in-title check (wta==YES already excluded above)
    $pdoc = ""
    $pMatch = $false
    try {
      $p = [Runtime.InteropServices.Marshal]::GetActiveObject('KWPP.Application')
      $pdoc = $p.ActivePresentation.Name
      $base = Split-Path $pdoc -Leaf
      $stem = $base
      if ($base -match '^(.+)\.[^\.]+$') { $stem = $Matches[1] }
      if ($stem.Length -ge 2) {
        $t = $ttl.ToLower()
        $pMatch = ($t.Contains($base.ToLower()) -or $t.Contains($stem.ToLower()))
      } else { $pMatch = $true }
    } catch { $pdoc = "" }
    if ($pdoc -ne "" -and $pMatch) {
      $line = "$line verdict=COM->ppt[$pdoc] (wta=$wta)"
    } else {
      $line = "$line verdict=no-COM(fallback) (wta=$wta ppt=$pdoc match=$pMatch)"
    }
  }
  $line = "$line FG: $cls [$ttl]"
  Add-Content -Path $log -Value $line -Encoding UTF8
  Start-Sleep -Milliseconds 500
}
Add-Content -Path $log -Value "SAMPLING DONE" -Encoding UTF8
