#include "stdafx.h"

#include <WeaselIPCData.h>
#include <thread>
#include <vector>
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
  TSFDbgLog(L"WeaselTSF ctor");
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
  TSFDbgLog(L"ActivateEx enter");
  com_ptr<ITfDocumentMgr> pDocMgrFocus;
  _activateFlags = dwFlags;

  _pThreadMgr = pThreadMgr;
  _tfClientId = tfClientId;

  if (!_InitThreadMgrEventSink()) {
    TSFDbgLog(L"ActivateEx fail: ThreadMgrEventSink");
    goto ExitError;
  }

  if ((_pThreadMgr->GetFocus(&pDocMgrFocus) == S_OK) &&
      (pDocMgrFocus != NULL)) {
    _InitTextEditSink(pDocMgrFocus);
  }

  if (!_InitKeyEventSink()) {
    TSFDbgLog(L"ActivateEx fail: KeyEventSink");
    goto ExitError;
  }

  // if (!_InitDisplayAttributeGuidAtom())
  //	goto ExitError;
  //	some app might init failed because it not provide DisplayAttributeInfo,
  // like some opengl stuff
  _InitDisplayAttributeGuidAtom();

  if (!_InitPreservedKey()) {
    TSFDbgLog(L"ActivateEx fail: PreservedKey");
    goto ExitError;
  }

  if (!_InitLanguageBar()) {
    TSFDbgLog(L"ActivateEx fail: LanguageBar");
    goto ExitError;
  }

  if (!_IsKeyboardOpen())
    _SetKeyboardOpen(TRUE);

  if (!_InitCompartment()) {
    TSFDbgLog(L"ActivateEx fail: Compartment");
    goto ExitError;
  }
  if (!_InitThreadFocusSink()) {
    TSFDbgLog(L"ActivateEx fail: ThreadFocusSink");
    goto ExitError;
  }

  TSFDbgLog(L"ActivateEx before EnsureServerConnected");
  _EnsureServerConnected();
  TSFDbgLog(L"ActivateEx after EnsureServerConnected");

  return S_OK;

ExitError:
  Deactivate();
  return E_FAIL;
}

STDMETHODIMP WeaselTSF::OnSetThreadFocus() {
  TSFDbgLog(L"OnSetThreadFocus enter");
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
  // 2026-08-11 二分: 移除 ResetContext/_RequestContextText — WPS 中
  // OnSetThreadFocus 频繁触发, 同步 IPC/EditSession 请求可能致 text service
  // 停用 (英文直出)。上下文采集主路径暂时停用, 待定位后以异步方式恢复。
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
  TSFDbgLog(L"Reconnect: Disconnect");
  m_client.Disconnect();
  TSFDbgLog(L"Reconnect: Connect");
  m_client.Connect(NULL);
  TSFDbgLog(L"Reconnect: StartSession");
  m_client.StartSession();
  TSFDbgLog(L"Reconnect: GetResponseData");
  weasel::ResponseParser parser(NULL, NULL, &_status, NULL, &_cand->style());
  bool ok = m_client.GetResponseData(std::ref(parser));
  TSFDbgLog(L"Reconnect: GetResponseData done ok=%d", (int)ok);
  if (ok) {
    _UpdateLanguageBar(_status);
  }
}

static unsigned int retry = 0;

bool WeaselTSF::_EnsureServerConnected() {
  TSFDbgLog(L"EnsureServerConnected enter");
  if (!m_client.Echo()) {
    TSFDbgLog(L"EnsureServerConnected Echo fail, Reconnect");
    _Reconnect();
    TSFDbgLog(L"EnsureServerConnected after Reconnect");
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
    TSFDbgLog(L"EnsureServerConnected Echo ok");
    return true;
  }
}

// 光标前文本获取: 文档锁内 GetSelection → 文档起点→光标 range → GetText
class CGetTextBeforeCaretEditSession : public CEditSession {
 public:
  CGetTextBeforeCaretEditSession(com_ptr<WeaselTSF> pTextService,
                                 com_ptr<ITfContext> pContext,
                                 bool immediate = false)
      : CEditSession(pTextService, pContext), immediate_(immediate) {}

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
    // 主路径: ShiftStart(-64) 负移 (微软官方标准做法, O(1) 轻量, TSF-aware
    // 应用 (Office/WPS/完整 TSF 实现) 下直接拿到光标前 ≤64 字符)。
    // 判据: 负移后起点 ≠ 文档起点 = 真的移动了 (transitory context 不
    // honored 锚点移动 / 起点已在 doc_start 被边界 clamp 时不移动)
    // → 回退 MOVESTART 大块方案。
    // 08-11 教训: Office/WPS 中 ShiftStart 负移不移动但不挂起; 挂起的是
    // MOVESTART 不被 honored 时的死循环 — 兜底方案有迭代上限防挂起。
    const ULONG kChunk = 8192;
    const int kMaxIter = 128;
    std::wstring text;
    bool used_neg_shift = false;
    {
      com_ptr<ITfRange> pNeg;
      if (pStart->Clone(&pNeg) == S_OK) {
        HRESULT hrNeg = pNeg->ShiftStart(ec, -kMaxCtxChars, nullptr, nullptr);
        BOOL equalStart = TRUE;
        pNeg->IsEqualStart(ec, pStart.p, TF_ANCHOR_START, &equalStart);
        if (SUCCEEDED(hrNeg) && !equalStart) {
          WCHAR buf[128] = {0};
          ULONG got = 0;
          if (pNeg->GetText(ec, 0, buf, 128, &got) == S_OK && got > 0) {
            text.assign(buf, (size_t)(got < (ULONG)kMaxCtxChars
                                          ? got
                                          : (ULONG)kMaxCtxChars));
            used_neg_shift = true;
          }
        }
      }
    }
    if (!used_neg_shift) {
      // 兜底: 大块 GetText (TF_TF_MOVESTART 推进起点), 全文后截尾部
      // kMaxCtxChars (光标前最近字符)。短文档一次取完 (got < 块大小即
      // 到底), 超长文档才追加下一块。防死循环: 起点不推进时 got 恒 =
      // 块大小 → 迭代上限 128 (≤1MB, 截尾 64 绰绰有余), 超限放弃本轮。
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
    }
    TSFDbgLog(L"CtxEditSession done chars=%d neg_shift=%d", (int)text.size(),
              (int)used_neg_shift);
    _pTextService->_OnContextTextReady(text, immediate_);
    return S_OK;
  }

 private:
  bool immediate_;  // 提交路径: 跳过去抖延迟立即发送 (第二词需在首键前送达)
};

void WeaselTSF::_RequestContextText(ITfContext* pContext, bool immediate) {
  TSFDbgLog(L"RequestContextText immediate=%d", (int)immediate);
  if (!pContext)
    return;
  com_ptr<CGetTextBeforeCaretEditSession> pEditSession(
      new CGetTextBeforeCaretEditSession(this, pContext, immediate));
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

void WeaselTSF::_OnContextTextReady(const std::wstring& text, bool immediate) {
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
    std::thread([this, immediate, utf8]() {
      // 提交路径 (immediate): 提交后立即发送, 下一词首键前上文已到位
      // (第二词重排用 TSF 上文而非历史回退); 普通文档变化保持 100ms
      // 合并 (CEF 类应用启动/滚动风暴防护)。
      // 空文本 (光标前无字符) 延长到 800ms: 打字期间 TSF 的 selection-change
      // 会频繁触发采集且瞬间 selection 未稳定 → 拿到 transient 空 (chars=0)
      // → 立即发送会覆盖服务端正常文本 (实测词 2 变历史/词 3 无标记)。
      // 800ms 窗口内新文本 (提交/文档变化采集) 会覆盖 pending, 真空
      // (光标真在开头) 800ms 后仍空才送达 (WPS 的 transient 空窗口
      // 比记事本更长, 2026-08-14 实测 TSF/历史交叉, 延长窗口减少误送达)。
      int delay = immediate ? 0 : (utf8.empty() ? 800 : 100);
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      for (;;) {
        std::string send;
        {
          std::lock_guard<std::mutex> lock(m_ctx_debounce_mutex);
          send = m_ctx_pending;
          m_ctx_last_sent = send;
        }
        // 空文本有语义 ("光标前无文本"): 必须发送, 否则服务端缓存
        // 保持旧文本 → 光标移到开头仍用旧上文重排 (2026-08-13 实测)。
        // SetContextText 空 body 服务端转 set_context_text("")
        // → 清缓存 + gen++ (librime 侧), 去重靠服务端 t==g_context_text。
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
