#pragma once

#include "Globals.h"
#include <WeaselIPC.h>
#include <WeaselIPCData.h>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

class CCandidateList;
class CLangBarItemButton;
class CCompartmentEventSink;

class WeaselTSF : public ITfTextInputProcessorEx,
                  public ITfThreadMgrEventSink,
                  public ITfTextEditSink,
                  public ITfTextLayoutSink,
                  public ITfKeyEventSink,
                  public ITfCompositionSink,
                  public ITfThreadFocusSink,
                  public ITfActiveLanguageProfileNotifySink,
                  public ITfEditSession,
                  public ITfDisplayAttributeProvider {
 public:
  WeaselTSF();
  ~WeaselTSF();

  /* IUnknown */
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject);
  STDMETHODIMP_(ULONG) AddRef();
  STDMETHODIMP_(ULONG) Release();

  /* ITfTextInputProcessor */
  STDMETHODIMP Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId);
  STDMETHODIMP Deactivate();

  /* ITfTextInputProcessorEx */
  STDMETHODIMP ActivateEx(ITfThreadMgr* pThreadMgr,
                          TfClientId tfClientId,
                          DWORD dwFlags);

  /* ITfThreadMgrEventSink */
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pDocMgr);
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr);
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                          ITfDocumentMgr* pDocMgrPrevFocus);
  STDMETHODIMP OnPushContext(ITfContext* pContext);
  STDMETHODIMP OnPopContext(ITfContext* pContext);

  /* ITfTextEditSink */
  STDMETHODIMP OnEndEdit(ITfContext* pic,
                         TfEditCookie ecReadOnly,
                         ITfEditRecord* pEditRecord);

  /* ITfTextLayoutSink */
  STDMETHODIMP OnLayoutChange(ITfContext* pContext,
                              TfLayoutCode lcode,
                              ITfContextView* pContextView);

  /* ITfKeyEventSink */
  STDMETHODIMP OnSetFocus(BOOL fForeground);
  STDMETHODIMP OnTestKeyDown(ITfContext* pContext,
                             WPARAM wParam,
                             LPARAM lParam,
                             BOOL* pfEaten);
  STDMETHODIMP OnKeyDown(ITfContext* pContext,
                         WPARAM wParam,
                         LPARAM lParam,
                         BOOL* pfEaten);
  STDMETHODIMP OnTestKeyUp(ITfContext* pContext,
                           WPARAM wParam,
                           LPARAM lParam,
                           BOOL* pfEaten);
  STDMETHODIMP OnKeyUp(ITfContext* pContext,
                       WPARAM wParam,
                       LPARAM lParam,
                       BOOL* pfEaten);
  STDMETHODIMP OnPreservedKey(ITfContext* pContext,
                              REFGUID rguid,
                              BOOL* pfEaten);

  // ITfThreadFocusSink
  STDMETHODIMP OnSetThreadFocus();
  STDMETHODIMP OnKillThreadFocus();

  /* ITfCompositionSink */
  STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite,
                                       ITfComposition* pComposition);

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

  /* ITfActiveLanguageProfileNotifySink */
  STDMETHODIMP OnActivated(REFCLSID clsid,
                           REFGUID guidProfile,
                           BOOL isActivated);

  // ITfDisplayAttributeProvider
  STDMETHODIMP EnumDisplayAttributeInfo(
      __RPC__deref_out_opt IEnumTfDisplayAttributeInfo** ppEnum);
  STDMETHODIMP GetDisplayAttributeInfo(
      __RPC__in REFGUID guidInfo,
      __RPC__deref_out_opt ITfDisplayAttributeInfo** ppInfo);

  ///* ITfCompartmentEventSink */
  // STDMETHODIMP OnChange(_In_ REFGUID guid);

  /* Compartments */
  BOOL _IsKeyboardDisabled();
  BOOL _IsKeyboardOpen();
  HRESULT _SetKeyboardOpen(BOOL fOpen);
  HRESULT _GetCompartmentDWORD(DWORD& value, const GUID guid);
  HRESULT _SetCompartmentDWORD(const DWORD& value, const GUID guid);

  /* Composition */
  void _StartComposition(com_ptr<ITfContext> pContext,
                         BOOL fCUASWorkaroundEnabled);
  void _EndComposition(com_ptr<ITfContext> pContext, BOOL clear);
  BOOL _ShowInlinePreedit(com_ptr<ITfContext> pContext,
                          const std::shared_ptr<weasel::Context> context);
  void _UpdateComposition(com_ptr<ITfContext> pContext);
  BOOL _IsComposing();
  void _SetComposition(com_ptr<ITfComposition> pComposition);
  void _SetCompositionPosition(const RECT& rc);
  BOOL _UpdateCompositionWindow(com_ptr<ITfContext> pContext);
  void _FinalizeComposition();
  void _AbortComposition(bool clear = true);

  /* Language bar */
  HWND _GetFocusedContextWindow();
  void _HandleLangBarMenuSelect(UINT wID);

  /* IPC */
  bool _EnsureServerConnected();

  /* UI */
  void _UpdateUI(const weasel::Context& ctx, const weasel::Status& status);
  void _StartUI();
  void _EndUI();
  void _ShowUI();
  void _HideUI();
  com_ptr<ITfContext> _GetUIContextDocument();

  /* Display Attribute */
  void _ClearCompositionDisplayAttributes(TfEditCookie ec,
                                          _In_ ITfContext* pContext);
  BOOL _SetCompositionDisplayAttributes(TfEditCookie ec,
                                        _In_ ITfContext* pContext,
                                        ITfRange* pRangeComposition);
  BOOL _InitDisplayAttributeGuidAtom();

  com_ptr<ITfThreadMgr> _GetThreadMgr() { return _pThreadMgr; }
  void HandleUICallback(size_t* const sel,
                        size_t* const hov,
                        bool* const next,
                        bool* const scroll_next);

 private:
  /* ui callback functions private */
  void _SelectCandidateOnCurrentPage(const size_t index);
  void _HandleMouseHoverEvent(const size_t index);
  void _HandleMousePageEvent(bool* const nextPage, bool* const scrollNextPage);
  /* TSF Related */
  BOOL _InitThreadMgrEventSink();
  void _UninitThreadMgrEventSink();
  // ITfThreadFocusSink
  BOOL _InitThreadFocusSink();
  void _UninitThreadFocusSink();
  DWORD _dwThreadFocusSinkCookie;

  BOOL _InitTextEditSink(com_ptr<ITfDocumentMgr> pDocMgr);

  BOOL _InitKeyEventSink();
  void _UninitKeyEventSink();
  void _ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten);
  // 按键时获取光标前文本 (TSF 文档锁内) 并发送给 server (供 LLM 上文)
  friend class CGetTextBeforeCaretEditSession;
  friend class CEndCompositionEditSession;  // 提交后立即采集 (下一词 TSF 上文)
  void _RequestContextText(ITfContext* pContext, bool immediate = false);
  void _OnContextTextReady(const std::wstring& text, bool immediate = false);
  // 架构调研 A 探针 (2026-08-18, 实验代码): 枚举 DocumentMgr 全部 context
  // 逐个试读光标前文本, 仅写日志不发送 — 验证 WPS 全文是否藏在非顶层
  // context (顶层只暴露 composition)。结论出来后移除或产品化
  void _ProbeAllContexts(ITfContext* pTopContext);
  // 上文采集去抖 (CEF 类应用文档更新极频繁, 限流 SetContextText 避免
  // IPC + Server 端 prepare 风暴): 非空 100ms 合并, 相同不重发, 空文本
  // 一律 800ms (WPS 提交后 transient 空高发, 详见 _OnContextTextReady)
  std::mutex m_ctx_debounce_mutex;
  std::condition_variable m_ctx_cv;  // 新 pending 唤醒等待中的 flush 线程 (空等待被真文本打断)
  std::string m_ctx_pending;        // 待发送的最新文本 (UTF-8)
  std::string m_ctx_last_sent;      // 已发送的文本
  bool m_ctx_flush_running = false;  // 合并发送线程是否在跑
  ULONGLONG m_ctx_pending_since = 0;  // pending 入队 tick (期间更新不重置, 保证发送进度)
  bool m_ctx_pending_immediate = false;  // pending 由提交路径产生 (非空 0ms 发送)
  uint64_t m_ctx_seq = 0;  // pending 更新计数 (等待谓词: 有新内容则唤醒重算)
  // focus:switch reset 1s 合并节流 (新版 WPS 内部 DocumentMgr 频繁 A↔B
  // 切换, 每次全清上下文+fallback → 风暴期词无重排; 见 ThreadMgrEventSink)
  ULONGLONG m_last_focus_reset_tick = 0;
  // ResetContext (编辑键/切窗) 后调用: 清空已发送标记, 强制下次采集重发
  // (否则 Server 端上下文被清空后, 相同文本因去抖被跳过永不重发 -> 无推理)
  void _OnContextReset();
  // 编辑键 (退格/删除/导航/回车, composition 空时): 光标位置变化,
  // 上屏历史不再代表光标前上文 -> 通知 librime 重置 commit-history 兜底
  void _HandleEditKeyReset(WPARAM wParam);
  std::string _Utf8FromWide(const std::wstring& wstr) const;
  std::wstring m_textBeforeCaret;

  BOOL _InitPreservedKey();
  void _UninitPreservedKey();

  BOOL _InitLanguageBar();
  void _UninitLanguageBar();
  void _UpdateLanguageBar(weasel::Status stat);
  void _ShowLanguageBar(BOOL show);
  void _EnableLanguageBar(BOOL enable);

  BOOL _InsertText(com_ptr<ITfContext> pContext, const std::wstring& ext);

  void _DeleteCandidateList();

  BOOL _InitCompartment();
  void _UninitCompartment();
  HRESULT _HandleCompartment(REFGUID guidCompartment);

  void _Reconnect();
  std::wstring _GetRootDir();

  bool isImmersive() const {
    return (_activateFlags & TF_TMF_IMMERSIVEMODE) != 0;
  }

  com_ptr<ITfThreadMgr> _pThreadMgr;
  TfClientId _tfClientId;
  DWORD _dwThreadMgrEventSinkCookie;

  com_ptr<ITfContext> _pTextEditSinkContext;
  DWORD _dwTextEditSinkCookie, _dwTextLayoutSinkCookie;
  BYTE _lpbKeyState[256];
  BOOL _fTestKeyDownPending, _fTestKeyUpPending;

  com_ptr<ITfContext> _pEditSessionContext;
  std::wstring _editSessionText;

  com_ptr<CCompartmentEventSink> _pKeyboardCompartmentSink;
  com_ptr<CCompartmentEventSink> _pConvertionCompartmentSink;

  com_ptr<ITfComposition> _pComposition;

  com_ptr<CLangBarItemButton> _pLangBarButton;

  com_ptr<CCandidateList> _cand;

  LONG _cRef;  // COM ref count

  /* CUAS Candidate Window Position Workaround */
  BOOL _fCUASWorkaroundTested, _fCUASWorkaroundEnabled;

  /* Weasel Related */
  weasel::Client m_client;
  DWORD _activateFlags;

  /* IME status */
  weasel::Status _status;

  // guidatom for the display attibute.
  TfGuidAtom _gaDisplayAttributeInput;
  BOOL _async_edit = false;
  BOOL _committed = false;
  BOOL _isToOpenClose = false;
};
