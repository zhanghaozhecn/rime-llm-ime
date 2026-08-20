# deploy_update.ps1 - minimal in-place update of LLM binaries (no TSF re-register, no reboot)
# Only rime.dll + WeaselServer.exe differ this round; old files kept as .bak
$ErrorActionPreference = 'Stop'
$log = 'D:\rime-llm-ime\bin\deploy_update.log'
$dest = 'C:\Program Files\Rime\weasel-0.17.4'
try {
  taskkill /f /im WeaselServer.exe 2>$null | Out-Null
  Start-Sleep -Seconds 3
  Copy-Item "$dest\rime.dll" "$dest\rime.dll.bak" -Force
  Copy-Item "$dest\WeaselServer.exe" "$dest\WeaselServer.exe.bak" -Force
  Copy-Item 'D:\rime-llm-ime\bin\rime.dll' $dest -Force
  Copy-Item 'D:\rime-llm-ime\bin\WeaselServer.exe' $dest -Force
  'OK' | Set-Content $log -Encoding ascii
} catch {
  $_ | Out-String | Set-Content $log -Encoding ascii
}
