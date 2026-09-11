# probe_tab_active2.ps1 - round 4: multi-instance writer tabs. Each sample:
# which doc the CURRENT GetActiveObject('Kwps.Application') instance sees,
# its view-window root==fg / visibility. Purpose: does GetActiveObject follow
# the ACTIVE tab when multiple wps.exe instances are registered in ROT?
$log = "D:\tab_probe2.log"
Remove-Item $log -ErrorAction SilentlyContinue
Add-Type -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint ga);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
'@ -Name U32 -Namespace WP2
$end = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $end) {
  $line = Get-Date -Format "HH:mm:ss.fff"
  $fg = [WP2.U32]::GetForegroundWindow()
  try {
    $app = [Runtime.InteropServices.Marshal]::GetActiveObject('Kwps.Application')
    $w = $app.ActiveWindow
    $h = [IntPtr]$w.Hwnd
    $root = [WP2.U32]::GetAncestor($h, 2)
    $line = "$line attach sees=[$($w.Document.Name)] rootEqFg=$($root -eq $fg -and $fg -ne [IntPtr]::Zero) vis=$([WP2.U32]::IsWindowVisible($h))"
  } catch { $line = "$line attach: ERR" }
  $sb = New-Object System.Text.StringBuilder 256
  [void][WP2.U32]::GetClassName($fg, $sb, 256)
  $ttl = New-Object System.Text.StringBuilder 256
  [void][WP2.U32]::GetWindowText($fg, $ttl, 256)
  $line = "$line FG: $($sb.ToString()) [$($ttl.ToString())]"
  Add-Content -Path $log -Value $line -Encoding UTF8
  Start-Sleep -Milliseconds 500
}
Add-Content -Path $log -Value "SAMPLING DONE" -Encoding UTF8
