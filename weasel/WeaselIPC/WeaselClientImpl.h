#pragma once
#include <WeaselIPC.h>
#include <PipeChannel.h>
#include <mutex>

namespace weasel {

class ClientImpl {
 public:
  ClientImpl();
  ~ClientImpl();

  bool Connect(ServerLauncher const& launcher);
  void Disconnect();
  void ShutdownServer();
  void StartSession();
  void EndSession();
  void StartMaintenance();
  void EndMaintenance();
  bool Echo();
  bool ProcessKeyEvent(KeyEvent const& keyEvent);
  bool CommitComposition();
  bool ClearComposition();
  bool SelectCandidateOnCurrentPage(size_t index);
  bool HighlightCandidateOnCurrentPage(size_t index);
  bool ChangePage(bool backward);
  void UpdateInputPosition(RECT const& rc);
  // rime-llm-ime: 发送光标前上文文本 (可能从 TSF 采集的独立线程调用)
  void SendContextText(const std::string& utf8_text);
  // rime-llm-ime: 编辑键/窗口切换时重置 librime 上下文 (可能从独立线程调用)
  void SendContextReset(const char* reason = "");
  void FocusIn();
  void FocusOut();
  void TrayCommand(UINT menuId);
  bool GetResponseData(ResponseHandler const& handler);

  // rime-llm-ime: 管道串行化锁 —— TSF 采集发送线程与主按键线程共用同一
  // 管道连接 (0.17.4 的 PipeChannel 非线程安全, 无 master 版 TLS 重构),
  // 所有对外方法加锁保证互斥
  mutable std::mutex channel_mutex;

 protected:
  void _InitializeClientInfo();
  bool _WriteClientInfo();

  LRESULT _SendMessage(WEASEL_IPC_COMMAND Msg, DWORD wParam, DWORD lParam);

  bool _Connected() const { return channel.Connected(); }
  bool _Active() const { return channel.Connected() && session_id != 0; }

 private:
  UINT session_id;
  std::wstring app_name;
  bool is_ime;

  PipeChannel<PipeMessage> channel;
};

}  // namespace weasel