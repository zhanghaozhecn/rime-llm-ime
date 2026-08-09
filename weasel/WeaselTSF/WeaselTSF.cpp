#include "stdafx.h"

#include <WeaselIPCData.h>
#include <thread>
#include <shellapi.h>
#include <tlhelp32.h>
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "LanguageBar.h"
#include "Compartment.h"
#include "ResponseParser.h"
#include "EditSession.h"

static void error_message(const WCHAR* msg) {
  static DWORD next_tick = 0;
  DWORD now = GetTickCount();
  if (now > next_tick) {
    next_tick = now + 10000;  // (ms)
    MessageBox(NULL, msg, get_weasel_ime_name().c_str(), MB_ICONERROR | MB_OK);
  }
}

WeaselTSF::WeaselTSF() {
  _cRef = 1;

  _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;

  _dwTextEditSinkCookie = TF_INVALID_COOKIE;
  _dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
  _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;
  _fTestKeyDownPending = FALSE;
  _fTestKeyUpPending = FALSE;

  _fCUASWorkaroundTested = _fCUASWorkaroundEnabled = FALSE;

  _cand = new CCandidateList(this);

  DllAddRef();
}

WeaselTSF::~WeaselTSF() {
  DllRelease();
}

STDAPI WeaselTSF::QueryInterface(REFIID riid, void** ppvObject) {
  if (ppvObject == NULL)
    return E_INVALIDARG;

  *ppvObject = NULL;

  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfTextInputProcessor))
    *ppvObject = (ITfTextInputProcessor*)this;
  else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    *ppvObject = (ITfTextInputProcessorEx*)this;
  else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    *ppvObject = (ITfThreadMgrEventSink*)this;
  else if (IsEqualIID(riid, IID_ITfTextEditSink))
    *ppvObject = (ITfTextEditSink*)this;
  else if (IsEqualIID(riid, IID_ITfTextLayoutSink))
    *ppvObject = (ITfTextLayoutSink*)this;
  else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    *ppvObject = (ITfKeyEventSink*)this;
  else if (IsEqualIID(riid, IID_ITfCompositionSink))
    *ppvObject = (ITfCompositionSink*)this;
  else if (IsEqualIID(riid, IID_ITfEditSession))
    *ppvObject = (ITfEditSession*)this;
  else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
    *ppvObject = (ITfThreadFocusSink*)this;
  else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
    *ppvObject = (ITfDisplayAttributeProvider*)this;

  if (*ppvObject) {
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

STDAPI_(ULONG) WeaselTSF::AddRef() {
  return ++_cRef;
}

STDAPI_(ULONG) WeaselTSF::Release() {
  LONG cr = --_cRef;

  assert(_cRef >= 0);

  if (_cRef == 0)
    delete this;

  return cr;
}

STDAPI WeaselTSF::Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) {
  return ActivateEx(pThreadMgr, tfClientId, 0U);
}

STDAPI WeaselTSF::Deactivate() {
  m_client.EndSession();

  _InitTextEditSink(com_ptr<ITfDocumentMgr>());

  _UninitThreadMgrEventSink();

  _UninitKeyEventSink();
  _UninitPreservedKey();

  _UninitLanguageBar();

  _UninitCompartment();

  _UninitThreadMgrEventSink();

  _pThreadMgr = NULL;

  _tfClientId = TF_CLIENTID_NULL;

  _cand->DestroyAll();

  return S_OK;
}

STDAPI WeaselTSF::ActivateEx(ITfThreadMgr* pThreadMgr,
                             TfClientId tfClientId,
                             DWORD dwFlags) {
  com_ptr<ITfDocumentMgr> pDocMgrFocus;
  _activateFlags = dwFlags;

  _pThreadMgr = pThreadMgr;
  _tfClientId = tfClientId;

  if (!_InitThreadMgrEventSink())
    goto ExitError;

  if ((_pThreadMgr->GetFocus(&pDocMgrFocus) == S_OK) &&
      (pDocMgrFocus != NULL)) {
    _InitTextEditSink(pDocMgrFocus);
  }

  if (!_InitKeyEventSink())
    goto ExitError;

  // if (!_InitDisplayAttributeGuidAtom())
  //	goto ExitError;
  //	some app might init failed because it not provide DisplayAttributeInfo,
  // like some opengl stuff
  _InitDisplayAttributeGuidAtom();

  if (!_InitPreservedKey())
    goto ExitError;

  if (!_InitLanguageBar())
    goto ExitError;

  if (!_IsKeyboardOpen())
    _SetKeyboardOpen(TRUE);

  if (!_InitCompartment())
    goto ExitError;
  if (!_InitThreadFocusSink())
    goto ExitError;

  _EnsureServerConnected();

  return S_OK;

ExitError:
  Deactivate();
  return E_FAIL;
}

STDMETHODIMP WeaselTSF::OnSetThreadFocus() {
  std::wstring _ToggleImeOnOpenClose{};
  RegGetStringValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                    L"ToggleImeOnOpenClose", _ToggleImeOnOpenClose);
  _isToOpenClose = (_ToggleImeOnOpenClose == L"yes");
  if (m_client.Echo()) {
    m_client.ProcessKeyEvent(0);
    weasel::ResponseParser parser(NULL, NULL, &_status, NULL, &_cand->style());
    bool ok = m_client.GetResponseData(std::ref(parser));
    if (ok)
      _UpdateLanguageBar(_status);
  }
  // Window switched (thread focus gained): reset the commit-history
  // fallback (session-level history no longer represents the caret
  // context), then asynchronously refresh the caret context text so the
  // LLM context is bounded to the new window.
  m_client.ResetContext("window:focus");
  _OnContextReset();  // 清去抖标记, 下次采集强制重发
  com_ptr<ITfDocumentMgr> pDocMgrFocus;
  if (_pThreadMgr && _pThreadMgr->GetFocus(&pDocMgrFocus) == S_OK &&
      pDocMgrFocus) {
    com_ptr<ITfContext> pContext;
    if (pDocMgrFocus->GetTop(&pContext) == S_OK && pContext) {
      _RequestContextText(pContext);
    }
  }
  return S_OK;
}
STDMETHODIMP WeaselTSF::OnKillThreadFocus() {
  _AbortComposition();
  return S_OK;
}
BOOL WeaselTSF::_InitThreadFocusSink() {
  com_ptr<ITfSource> pSource;
  if (FAILED(_pThreadMgr->QueryInterface(&pSource)))
    return FALSE;
  if (FAILED(pSource->AdviseSink(IID_ITfThreadFocusSink,
                                 (ITfThreadFocusSink*)this,
                                 &_dwThreadFocusSinkCookie)))
    return FALSE;
  return TRUE;
}
void WeaselTSF::_UninitThreadFocusSink() {
  com_ptr<ITfSource> pSource;
  if (FAILED(_pThreadMgr->QueryInterface(&pSource)))
    return;
  if (FAILED(pSource->UnadviseSink(_dwThreadFocusSinkCookie)))
    return;
}

STDMETHODIMP WeaselTSF::OnActivated(REFCLSID clsid,
                                    REFGUID guidProfile,
                                    BOOL isActivated) {
  if (!IsEqualCLSID(clsid, c_clsidTextService)) {
    return S_OK;
  }

  if (isActivated) {
    _ShowLanguageBar(TRUE);
    _UpdateLanguageBar(_status);
  } else {
    _DeleteCandidateList();
    _ShowLanguageBar(FALSE);
  }
  return S_OK;
}

void WeaselTSF::_Reconnect() {
  m_client.Disconnect();
  m_client.Connect(NULL);
  m_client.StartSession();
  weasel::ResponseParser parser(NULL, NULL, &_status, NULL, &_cand->style());
  bool ok = m_client.GetResponseData(std::ref(parser));
  if (ok) {
    _UpdateLanguageBar(_status);
  }
}

static unsigned int retry = 0;

bool WeaselTSF::_EnsureServerConnected() {
  if (!m_client.Echo()) {
    _Reconnect();
    retry++;
    if (retry >= 6) {
      HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerExclusiveMutex");
      const auto count_server_process = []() -> int {
        int count = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
          return 0;
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
          do {
            if (_wcsicmp(pe.szExeFile, L"WeaselServer.exe") == 0)
              count++;
          } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        return count;
      };
      if (!m_client.Echo() && GetLastError() != ERROR_ALREADY_EXISTS &&
          !count_server_process()) {
        std::wstring dir = _GetRootDir();
        std::thread th([dir, this]() {
          ShellExecuteW(NULL, L"open", (dir + L"\\start_service.bat").c_str(),
                        NULL, dir.c_str(), SW_HIDE);
          // wait 500ms, then reconnect
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          _Reconnect();
        });
        th.detach();
      }
      if (hMutex) {
        CloseHandle(hMutex);
      }
      retry = 0;
    }
    return (m_client.Echo() != 0);
  } else {
    return true;
  }
}

// 光标前文本获取: 文档锁内 GetSelection → 文档起点→光标 range → GetText
class CGetTextBeforeCaretEditSession : public CEditSession {
 public:
  CGetTextBeforeCaretEditSession(com_ptr<WeaselTSF> pTextService,
                                 com_ptr<ITfContext> pContext)
      : CEditSession(pTextService, pContext) {}

  STDMETHODIMP DoEditSession(TfEditCookie ec) {
    TF_SELECTION selection;
    ULONG nSelection = 0;
    HRESULT hrSel = _pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1,
                                            &selection, &nSelection);
    if (FAILED(hrSel) || nSelection == 0) {
      return E_FAIL;
    }
    // 只取光标前最近 kMaxCtxChars 字符 (LLM 上文 20 token x 1-2 字/token
    // = 40 字, 64 绰绰有余; 长文档时取开头而非光标前是错误的)
    const LONG kMaxCtxChars = 64;
    com_ptr<ITfRange> pStart;
    HRESULT hrStart = _pContext->GetStart(ec, &pStart);
    if (FAILED(hrStart))
      return E_FAIL;
    com_ptr<ITfRange> pTextRange;
    HRESULT hrClone = pStart->Clone(&pTextRange);
    if (FAILED(hrClone))
      return E_FAIL;
    // 文档起点 → 光标 (selection.range 是光标处的空 range)
    HRESULT hrShift =
        pTextRange->ShiftEndToRange(ec, selection.range, TF_ANCHOR_END);
    if (FAILED(hrShift))
      return E_FAIL;
    // 向前扩展 kMaxCtxChars: range = [caret-64, caret]。
    // 注意: 不能从光标处空 range 起 ShiftStart 负值 (实测返回 0 字符);
    // 从 [doc_start, caret] 起移动, 失败时保持原 range (短文档仍正确)
    LONG shifted = 0;
    pTextRange->ShiftStart(ec, -kMaxCtxChars, &shifted, NULL);
    WCHAR buf[64];
    ULONG got = 0;
    HRESULT hrText = pTextRange->GetText(ec, 0, buf, kMaxCtxChars, &got);
    if (FAILED(hrText)) {
      return E_FAIL;
    }
    std::wstring text(buf, got);
    _pTextService->_OnContextTextReady(text);
    return S_OK;
  }
};

void WeaselTSF::_RequestContextText(ITfContext* pContext) {
  if (!pContext)
    return;
  com_ptr<CGetTextBeforeCaretEditSession> pEditSession(
      new CGetTextBeforeCaretEditSession(this, pContext));
  HRESULT hr = E_FAIL;
  // 只读锁 + 异步 (按键时锁可能被占用; 缺 TF_ES_READ 权限位会直接 E_FAIL)
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_READ | TF_ES_ASYNCDONTCARE, &hr);
}

void WeaselTSF::_OnContextReset() {
  // Server 端上下文已清空 (RimeResetContextText): 清掉"已发送"标记,
  // 下次采集即使文本相同也会重发, 否则去抖跳过 -> Server 上下文永远空
  std::lock_guard<std::mutex> lock(m_ctx_debounce_mutex);
  m_ctx_last_sent.clear();
}

void WeaselTSF::_OnContextTextReady(const std::wstring& text) {
  m_textBeforeCaret = text;
  // 发送移出 TSF 文档锁 (EditSession 回调必须快速返回, 禁同步阻塞 IPC):
  // 独立线程执行管道 Transact (PipeChannel 线程安全, thread_local 管道句柄)。
  // 去抖: TSF 文档更新可极频繁 (CEF 类应用启动/滚动/歌词, 每次文档
  // 变化都触发采集), 每次直接发 SetContextText 会形成 IPC + Server 端
  // prepare 风暴, 拖慢其他应用启动 (QQ 音乐等打不开)。100ms 内合并为
  // 最新文本, 相同文本不重发, 发送期间的新更新循环补发。
  // (100ms 权衡: 300ms 时快速打字 [~100ms/键, 4 码 350ms] 下一词重排
  // 早于上文送达 -> ctx 空不重排 -> 词库顺序顶上屏错词 [试剂->事迹];
  // Server 端 on_context_changed 另有 300ms 节流 + ctx dedup 防 decode
  // 风暴, 客户端 100ms 只增轻量 IPC, QQ 音乐启动延迟不受影响)
  std::string utf8 = _Utf8FromWide(text);
  bool spawn = false;
  {
    std::lock_guard<std::mutex> lock(m_ctx_debounce_mutex);
    if (utf8 == m_ctx_last_sent)
      return;  // 相同文本不重发
    m_ctx_pending = utf8;
    if (m_ctx_flush_running)
      return;  // 已有合并线程, 它会发送最新值
    m_ctx_flush_running = true;
    spawn = true;
  }
  if (spawn) {
    std::thread([this]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      for (;;) {
        std::string send;
        {
          std::lock_guard<std::mutex> lock(m_ctx_debounce_mutex);
          send = m_ctx_pending;
          m_ctx_last_sent = send;
        }
        if (!send.empty())
          m_client.SetContextText(send);
        {
          std::lock_guard<std::mutex> lock(m_ctx_debounce_mutex);
          if (m_ctx_pending == m_ctx_last_sent) {
            m_ctx_flush_running = false;
            break;  // 发送期间无新更新, 收尾
          }
          // 发送期间又有新文本 → 循环补发最新值
        }
      }
    }).detach();
  }
}

std::string WeaselTSF::_Utf8FromWide(const std::wstring& wstr) const {
  if (wstr.empty())
    return "";
  int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                nullptr, 0, nullptr, nullptr);
  if (len <= 0)
    return "";
  std::string result(len, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0],
                      len, nullptr, nullptr);
  return result;
}
