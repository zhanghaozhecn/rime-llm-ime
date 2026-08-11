#include "stdafx.h"

#include <WeaselIPCData.h>
#include <thread>
#include <vector>
#include <shellapi.h>
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
      if (!m_client.Echo() && GetLastError() != ERROR_ALREADY_EXISTS) {
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

// ============================================================
// rime-llm-ime: 光标前上文采集
// 文档锁内 GetSelection -> 文档起点->光标 range -> GetText, 异步发送
// 给 server (SET_CONTEXT_TEXT IPC) 存入 librime, llm_filter 重排用。
// ============================================================
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
    // 文档起点 → 光标 (selection.range 是光标处的空 range)。
    // 注意: 不排除 composition —— WPS 的 composition 从文档开头延续
    // (连续输入时覆盖全文), composition 起点方案实测 ctx 恒空; 无条件
    // 读 [doc_start, caret] 是 WPS 唯一可用的采集方式 (编码残留由
    // librime 端 normalize_ctx 后处理 + 提交后采集的正确文本覆盖)。
    HRESULT hrShift =
        pTextRange->ShiftEndToRange(ec, selection.range, TF_ANCHOR_END);
    if (FAILED(hrShift))
      return E_FAIL;
    // 取 [doc_start, caret] 全文后截尾部 kMaxCtxChars (光标前最近字符)。
    // 大块 GetText (TF_TF_MOVESTART 推进起点): 短文档一次取完 (got < 块
    // 大小即到底), 超长文档才追加下一块。
    // 防死循环: 部分应用可能不 honored TF_TF_MOVESTART, 若起点不推进,
    // got 恒 = 块大小, 无限循环; 加迭代上限 128 (≤1MB 文本, 截尾 64
    // 字符绰绰有余), 超限放弃本轮采集 (上下文缺失不影响打字)。
    const ULONG kChunk = 8192;
    const int kMaxIter = 128;
    std::wstring text;
    std::vector<WCHAR> chunk(kChunk);
    for (int iter = 0; iter < kMaxIter; ++iter) {
      ULONG got = 0;
      HRESULT hrText = pTextRange->GetText(ec, TF_TF_MOVESTART, chunk.data(),
                                           kChunk, &got);
      if (FAILED(hrText)) {
        return E_FAIL;
      }
      if (got == 0)
        break;
      text.append(chunk.data(), got);
      if (got < kChunk)
        break;
    }
    if ((int)text.size() > kMaxCtxChars)
      text = text.substr(text.size() - kMaxCtxChars);
    _pTextService->_OnContextTextReady(text);
    return S_OK;
  }
};

bool WeaselTSF::_IsTSFCtxReliable() const {
  // WPS 系应用: Kso 定制 Qt 的 TSF 实现不完整 (composition 从文档开头
  // 延续/GetText 读到编码残留/提交采集失败), 实测采集结果脏且不稳定;
  // 禁用 TSF 上文采集 -> librime context_text_valid=false -> 自动退化
  // 到上屏历史 (commit history, 干净可靠)。
  WCHAR exe[MAX_PATH] = {0};
  GetModuleFileNameW(NULL, exe, MAX_PATH);
  _wcslwr_s(exe);
  if (wcsstr(exe, L"wps.exe") || wcsstr(exe, L"wpscloudsvr.exe") ||
      wcsstr(exe, L"wpscenter.exe") || wcsstr(exe, L"wpspdf.exe") ||
      wcsstr(exe, L"wpsupdate.exe"))
    return false;
  return true;
}

void WeaselTSF::_RequestContextText(ITfContext* pContext) {
  if (!pContext)
    return;
  if (!_IsTSFCtxReliable())
    return;  // WPS 系: 不采集, librime 退化历史上文
  com_ptr<CGetTextBeforeCaretEditSession> pEditSession(
      new CGetTextBeforeCaretEditSession(this, pContext));
  HRESULT hr = E_FAIL;
  // 只读锁 + 异步 (按键时锁可能被占用; 缺 TF_ES_READ 权限位会直接 E_FAIL)
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_READ | TF_ES_ASYNCDONTCARE, &hr);
}

void WeaselTSF::_OnContextTextReady(const std::wstring& text) {
  // 发送移出 TSF 文档锁 (EditSession 回调必须快速返回, 禁同步阻塞 IPC):
  // 独立线程执行管道 Transact (ClientImpl::channel_mutex 串行化管道访问)。
  // 去抖: TSF 文档更新可极频繁 (CEF 类应用启动/滚动/歌词, 每次文档
  // 变化都触发采集), 每次直接发 SetContextText 会形成 IPC + Server 端
  // prepare 风暴, 拖慢其他应用启动 (QQ 音乐等打不开)。100ms 内合并为
  // 最新文本, 相同文本不重发, 发送期间的新更新循环补发。
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
