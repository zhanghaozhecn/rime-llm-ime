#include "stdafx.h"
#include "WeaselClientImpl.h"
#include <StringAlgorithm.hpp>

using namespace weasel;

ClientImpl::ClientImpl()
    : session_id(0), channel(GetPipeName()), is_ime(false) {
  _InitializeClientInfo();
}

ClientImpl::~ClientImpl() {
  if (channel.Connected())
    Disconnect();
}

// http://stackoverflow.com/questions/557081/how-do-i-get-the-hmodule-for-the-currently-executing-code
HMODULE GetCurrentModule() {  // NB: XP+ solution!
  HMODULE hModule = NULL;
  GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    (LPCTSTR)GetCurrentModule, &hModule);

  return hModule;
}

void ClientImpl::_InitializeClientInfo() {
  // get app name
  WCHAR exe_path[MAX_PATH] = {0};
  GetModuleFileName(NULL, exe_path, MAX_PATH);
  std::wstring path = exe_path;
  size_t separator_pos = path.find_last_of(L"\\/");
  if (separator_pos < path.size())
    app_name = path.substr(separator_pos + 1);
  else
    app_name = path;
  to_lower(app_name);
  // determine client type
  GetModuleFileName(GetCurrentModule(), exe_path, MAX_PATH);
  path = exe_path;
  to_lower(path);
  is_ime = ends_with(path, L".ime");
}

// rime-llm-ime: 发送光标前上文文本 (UTF-8 -> 管道 body 宽字符流)
void ClientImpl::SendContextText(const std::string& utf8_text) {
  // 注意: 本方法可能从 TSF 回调的独立发送线程调用, 该线程可能尚未
  // StartSession (_Active() 检查会误判 false 而静默跳过), 因此不检查
  // _Active(); Transact 内部 _Ensure() 会按需建立连接。channel_mutex
  // 保证与主按键线程的管道访问互斥 (0.17.4 PipeChannel 非线程安全)。
  try {
    std::lock_guard<std::mutex> lock(channel_mutex);
    // 管道 body 流为 wbufferstream (宽字符), 需转 wstring 写入
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_text.c_str(),
                                   (int)utf8_text.size(), NULL, 0);
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_text.c_str(),
                        (int)utf8_text.size(), &wtext[0], wlen);
    channel.ClearBufferStream();
    channel.Write(wtext);
    PipeMessage req{WEASEL_IPC_SET_CONTEXT_TEXT, 0,
                    (DWORD)(wtext.size() * sizeof(wchar_t))};
    channel.Transact(req);
  } catch (...) {
  }
}

// rime-llm-ime: 编辑键/窗口切换: 通知 librime 重置上下文 (清空 + 递增代次)
void ClientImpl::SendContextReset(const char* reason) {
  try {
    std::lock_guard<std::mutex> lock(channel_mutex);
    channel.ClearBufferStream();
    if (reason && *reason) {
      int wlen = MultiByteToWideChar(CP_UTF8, 0, reason, (int)strlen(reason),
                                     NULL, 0);
      std::wstring wtext(wlen, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, reason, (int)strlen(reason), &wtext[0],
                          wlen);
      channel.Write(wtext);
      PipeMessage req{WEASEL_IPC_RESET_CONTEXT, 0,
                      (DWORD)(wtext.size() * sizeof(wchar_t))};
      channel.Transact(req);
    } else {
      PipeMessage req{WEASEL_IPC_RESET_CONTEXT, 0, 0};
      channel.Transact(req);
    }
  } catch (...) {
  }
}

bool ClientImpl::Connect(ServerLauncher const& launcher) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  return channel.Connect();
}

void ClientImpl::Disconnect() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (_Active())
    _SendMessage(WEASEL_IPC_END_SESSION, 0, session_id);  // 不调 EndSession 避免锁重入
  session_id = 0;
  channel.Disconnect();
}

void ClientImpl::ShutdownServer() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  _SendMessage(WEASEL_IPC_SHUTDOWN_SERVER, 0, 0);
}

bool ClientImpl::ProcessKeyEvent(KeyEvent const& keyEvent) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (!_Active())
    return false;

  LRESULT ret =
      _SendMessage(WEASEL_IPC_PROCESS_KEY_EVENT, keyEvent, session_id);
  return ret != 0;
}

bool ClientImpl::CommitComposition() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (!_Active())
    return false;

  LRESULT ret = _SendMessage(WEASEL_IPC_COMMIT_COMPOSITION, 0, session_id);
  return ret != 0;
}

bool ClientImpl::ClearComposition() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (!_Active())
    return false;

  LRESULT ret = _SendMessage(WEASEL_IPC_CLEAR_COMPOSITION, 0, session_id);
  return ret != 0;
}

bool ClientImpl::SelectCandidateOnCurrentPage(size_t index) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (!_Active())
    return false;
  LRESULT ret = _SendMessage(WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE, index,
                             session_id);
  return ret != 0;
}

bool ClientImpl::HighlightCandidateOnCurrentPage(size_t index) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (!_Active())
    return false;
  LRESULT ret = _SendMessage(WEASEL_IPC_HIGHLIGHT_CANDIDATE_ON_CURRENT_PAGE,
                             index, session_id);
  return ret != 0;
}

bool ClientImpl::ChangePage(bool backward) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (!_Active())
    return false;
  LRESULT ret = _SendMessage(WEASEL_IPC_CHANGE_PAGE, backward, session_id);
  return ret != 0;
}

void ClientImpl::UpdateInputPosition(RECT const& rc) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (!_Active())
    return;
  /*
  移位标志 = 1bit == 0
  height:0~127 = 7bit
  top:-2048~2047 = 12bit（有符号）
  left:-2048~2047 = 12bit（有符号）

  高解析度下：
  移位标志 = 1bit == 1
  height:0~254 = 7bit（舍弃低1位）
  top:-4096~4094 = 12bit（有符号，舍弃低1位）
  left:-4096~4094 = 12bit（有符号，舍弃低1位）
  */
  int hi_res =
      static_cast<int>(rc.bottom - rc.top >= 128 || rc.left < -2048 ||
                       rc.left >= 2048 || rc.top < -2048 || rc.top >= 2048);
  int left = max(-2048, min(2047, rc.left >> hi_res));
  int top = max(-2048, min(2047, rc.top >> hi_res));
  int height = max(0, min(127, (rc.bottom - rc.top) >> hi_res));
  DWORD compressed_rect = ((hi_res & 0x01) << 31) | ((height & 0x7f) << 24) |
                          ((top & 0xfff) << 12) | (left & 0xfff);
  _SendMessage(WEASEL_IPC_UPDATE_INPUT_POS, compressed_rect, session_id);
}

void ClientImpl::FocusIn() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  DWORD client_caps = 0; /* TODO */
  _SendMessage(WEASEL_IPC_FOCUS_IN, client_caps, session_id);
}

void ClientImpl::FocusOut() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  _SendMessage(WEASEL_IPC_FOCUS_OUT, 0, session_id);
}

void ClientImpl::TrayCommand(UINT menuId) {
  std::lock_guard<std::mutex> lock(channel_mutex);
  _SendMessage(WEASEL_IPC_TRAY_COMMAND, menuId, session_id);
}

void ClientImpl::StartSession() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (_Active() && Echo())
    return;

  _WriteClientInfo();
  UINT ret = _SendMessage(WEASEL_IPC_START_SESSION, 0, 0);
  session_id = ret;
}

void ClientImpl::EndSession() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  _SendMessage(WEASEL_IPC_END_SESSION, 0, session_id);
  session_id = 0;
}

void ClientImpl::StartMaintenance() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  _SendMessage(WEASEL_IPC_START_MAINTENANCE, 0, 0);
  session_id = 0;
}

void ClientImpl::EndMaintenance() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  _SendMessage(WEASEL_IPC_END_MAINTENANCE, 0, 0);
  session_id = 0;
}

bool ClientImpl::Echo() {
  std::lock_guard<std::mutex> lock(channel_mutex);
  if (!_Active())
    return false;

  UINT serverEcho = _SendMessage(WEASEL_IPC_ECHO, 0, session_id);
  return (serverEcho == session_id);
}

bool ClientImpl::GetResponseData(ResponseHandler const& handler) {
  if (!handler) {
    return false;
  }
  std::lock_guard<std::mutex> lock(channel_mutex);
  return channel.HandleResponseData(handler);
}

bool ClientImpl::_WriteClientInfo() {
  channel << L"action=session\n";
  channel << L"session.client_app=" << app_name.c_str() << L"\n";
  channel << L"session.client_type=" << (is_ime ? L"ime" : L"tsf") << L"\n";
  channel << L".\n";
  return true;
}

LRESULT ClientImpl::_SendMessage(WEASEL_IPC_COMMAND Msg,
                                 DWORD wParam,
                                 DWORD lParam) {
  try {
    PipeMessage req{Msg, wParam, lParam};
    return channel.Transact(req);
  } catch (DWORD /* ex */) {
    return 0;
  }
}

Client::Client() : m_pImpl(new ClientImpl()) {}

Client::~Client() {
  if (m_pImpl)
    delete m_pImpl;
}

bool Client::Connect(ServerLauncher launcher) {
  return m_pImpl->Connect(launcher);
}

void Client::Disconnect() {
  m_pImpl->Disconnect();
}

void Client::ShutdownServer() {
  m_pImpl->ShutdownServer();
}

bool Client::ProcessKeyEvent(KeyEvent const& keyEvent) {
  return m_pImpl->ProcessKeyEvent(keyEvent);
}

bool Client::CommitComposition() {
  return m_pImpl->CommitComposition();
}

bool Client::ClearComposition() {
  return m_pImpl->ClearComposition();
}

bool Client::SelectCandidateOnCurrentPage(size_t index) {
  return m_pImpl->SelectCandidateOnCurrentPage(index);
}

bool Client::HighlightCandidateOnCurrentPage(size_t index) {
  return m_pImpl->HighlightCandidateOnCurrentPage(index);
}

bool Client::ChangePage(bool backward) {
  return m_pImpl->ChangePage(backward);
}

void Client::UpdateInputPosition(RECT const& rc) {
  m_pImpl->UpdateInputPosition(rc);
}

void Client::SetContextText(const std::string& utf8_text) {
  m_pImpl->SendContextText(utf8_text);
}

void Client::ResetContext(const char* reason) {
  m_pImpl->SendContextReset(reason);
}

void Client::FocusIn() {
  m_pImpl->FocusIn();
}

void Client::FocusOut() {
  m_pImpl->FocusOut();
}

void Client::StartSession() {
  m_pImpl->StartSession();
}

void Client::EndSession() {
  m_pImpl->EndSession();
}

void Client::StartMaintenance() {
  m_pImpl->StartMaintenance();
}

void Client::EndMaintenance() {
  m_pImpl->EndMaintenance();
}

void Client::TrayCommand(UINT menuId) {
  m_pImpl->TrayCommand(menuId);
}

bool Client::Echo() {
  return m_pImpl->Echo();
}

bool Client::GetResponseData(ResponseHandler handler) {
  return m_pImpl->GetResponseData(handler);
}
