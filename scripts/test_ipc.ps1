# test_ipc.ps1 — 自动化管道级测试: 直接向 WeaselServer 发送 IPC 消息
# 用法:
#   pwsh -File test_ipc.ps1                          # RESET_CONTEXT "focus:switch" x1
#   pwsh -File test_ipc.ps1 -Msg 32785 -Body "你好"   # SET_CONTEXT_TEXT
#   pwsh -File test_ipc.ps1 -Times 5                  # 连发测试
# 消息号: 32785=SET_CONTEXT_TEXT 32786=RESET_CONTEXT 32772=PROCESS_KEY_EVENT
# 结果看服务端日志 %TEMP%\weasel_srv_dbg.log (RESET_IPC 行验证 body 完整性)
param(
  [int]$Msg = 32786,
  [string]$Body = "focus:switch",
  [int]$Times = 1
)
$pipeName = "Administrator\WeaselNamedPipe"  # \\.\pipe\<user>\WeaselNamedPipe
for ($i = 0; $i -lt $Times; $i++) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
  try {
    $pipe.Connect(3000)
  } catch {
    Write-Host "connect FAILED: $($_.Exception.Message)"
    exit 1
  }
  $bodyBytes = [Text.Encoding]::Unicode.GetBytes($Body)  # UTF-16LE = wchar_t
  $total = 12 + $bodyBytes.Length
  $data = New-Object "System.Byte[]" $total
  [BitConverter]::GetBytes([int32]$Msg).CopyTo($data, 0)
  [BitConverter]::GetBytes([int32]0).CopyTo($data, 4)
  [BitConverter]::GetBytes([int32]$bodyBytes.Length).CopyTo($data, 8)
  [Array]::Copy($bodyBytes, 0, $data, 12, $bodyBytes.Length)
  $pipe.Write($data, 0, $total)
  $pipe.Flush()
  $resp = New-Object "System.Byte[]" 4
  $null = $pipe.Read($resp, 0, 4)
  Write-Host ("msg={0} body={1} resp={2}" -f $Msg, $Body, [BitConverter]::ToInt32($resp, 0))
  $pipe.Dispose()
}
