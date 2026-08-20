# deploy_update.ps1 - minimal in-place update (only rime.dll + WeaselServer.exe).
# Use case: minor update where TSF DLLs (weasel32/weaselx64) are UNCHANGED vs
# the installed version -> no WeaselSetup re-register, no reboot needed.
# Source = this script's directory (works from bin\ or an extracted release zip).
# Old files are kept as .bak in the install dir for rollback.
$ErrorActionPreference = 'Stop'

# self-elevate: writing to Program Files requires admin
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`""
  exit
}

$src  = Split-Path $PSCommandPath -Parent
$dest = 'C:\Program Files\Rime\weasel-0.17.4'
if (-not (Test-Path $dest)) { throw "install dir not found: $dest" }

taskkill /f /im WeaselServer.exe 2>$null | Out-Null
Start-Sleep -Seconds 3

Copy-Item "$dest\rime.dll" "$dest\rime.dll.bak" -Force
Copy-Item "$dest\WeaselServer.exe" "$dest\WeaselServer.exe.bak" -Force
Copy-Item "$src\rime.dll" $dest -Force
Copy-Item "$src\WeaselServer.exe" $dest -Force

Start-Process "$dest\WeaselServer.exe" -WorkingDirectory $dest
Write-Host 'Update done: rime.dll + WeaselServer.exe replaced, server restarted.'
Write-Host 'No reboot / TSF re-registration needed (TSF DLLs unchanged).'
Read-Host 'Press Enter to close'
