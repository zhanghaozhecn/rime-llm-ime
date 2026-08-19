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
    // GetSelection 返回的 range 引用归调用方 (原实现未 Release, 每次采集
    // 泄漏一个 range 对象 — 21649 次采集即泄漏 21649 个); Attach 接管
    com_ptr<ITfRange> pCaret;
    pCaret.Attach(selection.range);
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
        pTextRange->ShiftEndToRange(ec, pCaret.p, TF_ANCHOR_END);
    if (FAILED(hrShift))
      return E_FAIL;
    // 主路径: 克隆光标 range → ShiftStart 负移 ≤64 字符 → [光标-64, 光标]
    // (微软官方标准做法, O(1) 轻量, TSF-aware 应用 (Office/WPS/完整 TSF
    // 实现) 下直接拿到光标前 ≤64 字符)。判据: 负移后起点 ≠ 原光标起点 =
    // 真的移动了 (transitory context 不 honored 锚点移动 / 光标已在文档
    // 开头被边界 clamp 时不移动) → 回退 MOVESTART 大块方案。
    // 2026-08-18 修复: 原实现克隆的是 pStart (文档起点), 从文档起点向负
    // 方向移动永远被钳住 → IsEqualStart 恒真 → 主路径从未生效 (日志
    // 20379/20379 次 neg_shift=0), 全部走 O(全文) 大块兜底 (采集更慢更
    // 易被拒: 8.4% 编辑会话静默丢弃; >1MB 文档取到错误文本)。负移的
    // 对象必须是光标 range, 不是文档起点。
    const ULONG kChunk = 8192;
    const int kMaxIter = 128;
    std::wstring text;
    bool used_neg_shift = false;
    {
      com_ptr<ITfRange> pNeg;
      if (pCaret->Clone(&pNeg) == S_OK) {
        // pcch 必须传实参: 传 nullptr 时 msctf 实现疑似 E_POINTER
        // (250/250 neg_shift=0 的疑因 — 与克隆对象修复无关, 新旧代码
        // 同款写法, 恰好掩盖了 pStart→pCaret 修复的效果)
        LONG cchMoved = 0;
        HRESULT hrNeg =
            pNeg->ShiftStart(ec, -kMaxCtxChars, &cchMoved, nullptr);
        BOOL equalStart = TRUE;
        pNeg->IsEqualStart(ec, pCaret.p, TF_ANCHOR_START, &equalStart);
        if (SUCCEEDED(hrNeg) && !equalStart) {
          WCHAR buf[128] = {0};
          ULONG got = 0;
          if (pNeg->GetText(ec, 0, buf, 128, &got) == S_OK && got > 0) {
            text.assign(buf, (size_t)(got < (ULONG)kMaxCtxChars
                                          ? got
                                          : (ULONG)kMaxCtxChars));
            used_neg_shift = true;
          }
        } else {
          // 诊断: 负移未生效的具体原因 (hr 失败 / 起点未移动=光标在文档
          // 开头属正常)。一次采集一行, 与既有日志量级相同
          TSFDbgLog(L"CtxNegShift fallback hr=0x%08X eq=%d",
                    (unsigned)hrNeg, (int)equalStart);
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
  // 被拒的编辑会话静默丢弃 (原实现 8.4% 请求无 "done" 日志), 至少留痕
  if (FAILED(hr))
    TSFDbgLog(L"RequestContextText: RequestEditSession denied hr=0x%08X",
              (unsigned)hr);
}

// ===== 架构调研 A 探针 (实验代码, 2026-08-18) =====
// WPS 顶层 context 只暴露 composition 区域 (连续打字只读到最近词)。枚举
// DocumentMgr 的 context 栈 (EnumContexts/GetBase), 逐个 context 试读
// "文档起点→光标" 全文, 结果只写日志 — 验证全文是否藏在非顶层 context。
// 触发: OnSetFocus (会话外, TSF 线程上, 安全); 2s 限流防焦点风暴刷屏。
class CProbeContextEditSession : public CEditSession {
 public:
  CProbeContextEditSession(com_ptr<WeaselTSF> pTextService,
                           com_ptr<ITfContext> pContext, int idx, bool isTop,
                           bool isBase)
      : CEditSession(pTextService, pContext),
        idx_(idx),
        isTop_(isTop),
        isBase_(isBase) {}

  STDMETHODIMP DoEditSession(TfEditCookie ec) {
    TF_SELECTION selection;
    ULONG nSelection = 0;
    HRESULT hrSel = _pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1,
                                            &selection, &nSelection);
    if (FAILED(hrSel) || nSelection == 0) {
      TSFDbgLog(L"CtxProbe[%d top=%d base=%d] no-selection hr=0x%08X", idx_,
                (int)isTop_, (int)isBase_, (unsigned)hrSel);
      return S_OK;
    }
    com_ptr<ITfRange> pCaret;
    pCaret.Attach(selection.range);
    com_ptr<ITfRange> pStart;
    if (FAILED(_pContext->GetStart(ec, &pStart))) {
      TSFDbgLog(L"CtxProbe[%d top=%d base=%d] no-start", idx_, (int)isTop_,
                (int)isBase_);
      return S_OK;
    }
    com_ptr<ITfRange> pRange;
    if (FAILED(pStart->Clone(&pRange)) ||
        FAILED(pRange->ShiftEndToRange(ec, pCaret.p, TF_ANCHOR_END))) {
      TSFDbgLog(L"CtxProbe[%d top=%d base=%d] range-fail", idx_, (int)isTop_,
                (int)isBase_);
      return S_OK;
    }
    // 简化读取: 16 块 x 8192 字符上限 (探针不需要与主路径同级的防御)
    std::wstring text;
    WCHAR chunk[8192];
    for (int i = 0; i < 16; ++i) {
      ULONG got = 0;
      if (FAILED(pRange->GetText(ec, TF_TF_MOVESTART, chunk, 8192, &got)))
        break;
      if (got == 0)
        break;
      text.append(chunk, got);
      if (got < 8192)
        break;
    }
    int total = (int)text.size();
    if (text.size() > 64)
      text = text.substr(text.size() - 64);
    TSFDbgLog(L"CtxProbe[%d top=%d base=%d] chars=%d tail=[%.24s]", idx_,
              (int)isTop_, (int)isBase_, total, text.c_str());
    return S_OK;
  }

 private:
  int idx_;
  bool isTop_, isBase_;
};

void WeaselTSF::_ProbeAllContexts(ITfContext* pTopContext) {
  // 2026-08-18 调研结论 (探针 81 次实测): WPS 的 DocumentMgr 仅 1 个
  // context (top==base), TSF store 只含 composition 区, 无全文; UIA 亦无
  // 文档正文 (Qt/KProme 树只有功能区控件, 读屏标志也不激活) — WPS 全文
  // 不可达, 现架构 (TSF + lagging→历史上文标 tsf) 即最优。探针保留但
  // 默认关闭, 新版 WPS 复查时置 true 重编。
  constexpr bool kCtxProbeEnabled = false;
  if (!kCtxProbeEnabled)
    return;
  static ULONGLONG s_last = 0;
  ULONGLONG now = GetTickCount64();
  if (s_last && now - s_last < 2000)
    return;  // 限流: WPS 焦点风暴下防刷屏
  s_last = now;
  com_ptr<ITfDocumentMgr> pMgr;
  if (FAILED(pTopContext->GetDocumentMgr(&pMgr)) || !pMgr)
    return;
  com_ptr<ITfContext> pTop, pBase;
  pMgr->GetTop(&pTop);
  pMgr->GetBase(&pBase);
  com_ptr<IEnumTfContexts> pEnum;
  if (FAILED(pMgr->EnumContexts(&pEnum)) || !pEnum) {
    TSFDbgLog(L"CtxProbe: EnumContexts failed");
    return;
  }
  TSFDbgLog(L"CtxProbe: begin enumeration");
  com_ptr<ITfContext> pCtx;
  int idx = 0;
  while (pEnum->Next(1, &pCtx, nullptr) == S_OK) {
    bool isTop = (pCtx.p == pTop.p);
    bool isBase = (pCtx.p == pBase.p);
    com_ptr<CProbeContextEditSession> pProbe(new CProbeContextEditSession(
        this, pCtx, idx, isTop, isBase));
    HRESULT hr = E_FAIL;
    pCtx->RequestEditSession(_tfClientId, pProbe,
                             TF_ES_READ | TF_ES_ASYNCDONTCARE, &hr);
    TSFDbgLog(L"CtxProbe[%d top=%d base=%d] requested hr=0x%08X", idx,
              (int)isTop, (int)isBase, (unsigned)hr);
    pCtx.Release();
    idx++;
    if (idx >= 8)
      break;  // 防御: 异常应用 context 过多
  }
  TSFDbgLog(L"CtxProbe: enumerated %d contexts", idx);
}

void WeaselTSF::_OnContextReset() {  // Server 端上下文已清空 (RimeResetContextText): 清掉"已发送"标记,
  // 下次采集即使文本相同也会重发, 否则去抖跳过 -> Server 上下文永远空
  std::lock_guard<std::mutex> lock(m_ctx_debounce_mutex);
  m_ctx_last_sent.clear();
}

void WeaselTSF::_OnContextTextReady(const std::wstring& text, bool immediate) {
  m_textBeforeCaret = text;
  // 发送移出 TSF 文档锁 (EditSession 回调必须快速返回, 禁同步阻塞 IPC):
  // 独立线程执行管道 Transact (PipeChannel 线程安全, thread_local 管道句柄)。
  // 去抖: TSF 文档更新可极频繁 (CEF 类应用启动/滚动/歌词, 每次文档变化都
  // 触发采集), 每次直接发 SetContextText 会形成 IPC + Server 端 prepare
  // 风暴, 拖慢其他应用启动 (QQ 音乐等打不开)。
  // 规则 (2026-08-18 重设计):
  //   非空 + 提交路径 (immediate): 0ms — 下一词首键前上文已到位;
  //   非空 + 普通文档变化: 100ms 合并 (自入队起算, 期间新更新不重置计时
  //     → 保证发送进度, 风暴下至多每 100ms 发一条);
  //   空文本: 一律 800ms (immediate 不绕过 — 提交后瞬间是 transient 空
  //     高发时刻)。空的后果由 librime 侧兜底: 受限应用 (WPS, lagging
  //     粘性降级) 整窗不采用 TSF 文本; 好应用本就极少送空 (2026-08-18
  //     八轮架构简化, 原 2s 活跃时钟抑制机制已删)。
  // (Server 端 on_context_changed 另有 300ms 节流 + ctx dedup 防 decode
  // 风暴, 客户端去抖只增轻量 IPC)
  std::string utf8 = _Utf8FromWide(text);
  bool spawn = false;
  {
    std::lock_guard<std::mutex> lock(m_ctx_debounce_mutex);
    if (utf8 == m_ctx_last_sent)
      return;  // 相同文本不重发
    if (m_ctx_pending == m_ctx_last_sent)
      m_ctx_pending_since = GetTickCount64();  // 队列从空到有才起算
    m_ctx_pending = utf8;
    if (immediate)
      m_ctx_pending_immediate = true;
    ++m_ctx_seq;
    if (!m_ctx_flush_running) {
      m_ctx_flush_running = true;
      spawn = true;
    }
  }
  m_ctx_cv.notify_all();
  if (spawn) {
    std::thread([this]() {
      for (;;) {
        std::string send;
        {
          std::unique_lock<std::mutex> lock(m_ctx_debounce_mutex);
          send = m_ctx_pending;
          // 简化去抖 (2026-08-18 八轮架构简化): 非空 immediate 0ms /
          // 普通 100ms 合并 (自入队起算, 期间更新不重置计时保证进度);
          // 空一律 800ms (immediate 不绕过 — 提交后瞬间是 transient 空
          // 高发时刻)。二~七轮的空送达抑制整套机制 (绝对截止时间/
          // 活跃时钟/按键时钟) 已删除: WPS 类受限应用在 librime 侧
          // lagging 粘性降级到历史上文后, 空送达不再有杀伤力; 好应用
          // 本就极少送空。等待结束 (超时或新 pending) 无条件回循环头
          // 重算 (六轮教训保留: 超时≠无变化)
          ULONGLONG quota =
              send.empty() ? 800 : (m_ctx_pending_immediate ? 0 : 100);
          ULONGLONG waited = GetTickCount64() - m_ctx_pending_since;
          int delay = waited >= quota ? 0 : (int)(quota - waited);
          if (delay > 0) {
            uint64_t seq = m_ctx_seq;
            m_ctx_cv.wait_for(lock, std::chrono::milliseconds(delay),
                              [&] { return m_ctx_seq != seq; });
            continue;  // 重读 pending 重算 (可能已更新)
          }
        }
        // 空文本有语义 ("光标前无文本"): 必须发送, 否则服务端缓存保持旧
        // 文本 → 光标移到开头仍用旧上文重排 (2026-08-13 实测)。
        // SetContextText 空 body 服务端转 set_context_text("");
        // 去重靠服务端 t==g_context_text (不清 fallback, 2026-08-13 撤回)。
        m_client.SetContextText(send);
        {
          std::lock_guard<std::mutex> lock(m_ctx_debounce_mutex);
          m_ctx_last_sent = send;
          if (m_ctx_pending == send) {
            m_ctx_pending_immediate = false;  // 标记随该内容一起消费
            m_ctx_flush_running = false;
            break;  // 发送期间无新更新, 收尾
          }
          // 发送期间又有新文本 → 循环补发最新值 (immediate 标记属新 pending)
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
