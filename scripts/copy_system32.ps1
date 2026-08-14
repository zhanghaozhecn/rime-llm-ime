# copy_system32.ps1 - copy latest 64-bit LLM build to System32 (elevated)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  Start-Process -FilePath "pwsh" -ArgumentList "-NoProfile", "-File", "`"$PSCommandPath`"" -Verb RunAs -Wait
  exit 0
}
Copy-Item (Join-Path $root 'weasel\output\weaselx64.dll') "C:\WINDOWS\System32\weasel.dll" -Force
Write-Host "System32 deployed: $((Get-FileHash 'C:\WINDOWS\System32\weasel.dll' -Algorithm MD5).Hash)"
