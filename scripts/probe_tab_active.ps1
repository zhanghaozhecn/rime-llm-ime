# probe_tab_active.ps1 - sample WPS component activity signals while user
# switches tabs: writer ActiveWindow.Active/Hwnd-visibility, KWPP same,
# plus foreground class/title. Purpose: find a name-independent signal for
# the ACTIVE tab component (2026-09-11 unified-tab cross-feed fix, round 3).
$log = "D:\tab_probe.log"
Remove-Item $log -ErrorAction SilentlyContinue
Add-Type -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
'@ -Name U32 -Namespace WP
$end = (Get-Date).AddSeconds(150)
while ((Get-Date) -lt $end) {
  $line = Get-Date -Format "HH:mm:ss.fff"
  try {
    $app = [Runtime.InteropServices.Marshal]::GetActiveObject('Kwps.Application')
    $win = $app.ActiveWindow
    $vis = [WP.U32]::IsWindowVisible([IntPtr]$win.Hwnd)
    $line = "$line writer: Active=$($win.Active) HwndVis=$vis"
  } catch { $line = "$line writer: ERR" }
  try {
    $app2 = [Runtime.InteropServices.Marshal]::GetActiveObject('KWPP.Application')
    $win2 = $app2.ActiveWindow
    try {
      $vis2 = [WP.U32]::IsWindowVisible([IntPtr]$win2.HWND)
      $line = "$line ppt: Active=$($win2.Active) HwndVis=$vis2"
    } catch { $line = "$line ppt: Active=$($win2.Active) HwndVis=N/A" }
  } catch { $line = "$line ppt: n/a" }
  $fg = [WP.U32]::GetForegroundWindow()
  $sb = New-Object System.Text.StringBuilder 256
  [void][WP.U32]::GetClassName($fg, $sb, 256)
  $ttl = New-Object System.Text.StringBuilder 256
  [void][WP.U32]::GetWindowText($fg, $ttl, 256)
  $line = "$line FG: $($sb.ToString()) [$($ttl.ToString())]"
  Add-Content -Path $log -Value $line -Encoding UTF8
  Start-Sleep -Milliseconds 500
}
Add-Content -Path $log -Value "SAMPLING DONE" -Encoding UTF8
