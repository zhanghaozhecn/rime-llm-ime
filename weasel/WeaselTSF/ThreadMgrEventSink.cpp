#include "stdafx.h"
#include "WeaselTSF.h"
#include <thread>

STDAPI WeaselTSF::OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) {
  return S_OK;
}

STDAPI WeaselTSF::OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) {
  return S_OK;
}

STDAPI WeaselTSF::OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                             ITfDocumentMgr* pDocMgrPrevFocus) {
  TSFDbgLog(L"OnSetFocusDocMgr enter");
  _InitTextEditSink(pDocMgrFocus);

  // rime-llm-ime: 窗口切换 (DocumentMgr 真正变化) → 服务端缓存的上文
  // 属于旧窗口, 异步 reset 防新窗口第一词用旧窗口文本重排 (残留检测
  // 互证失效场景: fallback 未清时旧 TSF 文本与旧 fallback 互证"通过")。
  // 仅 prevFocus != focus 才 reset: 同一 DocumentMgr 的重聚焦 (打字期间
  // 候选窗/输入焦点变化会频繁触发 OnSetFocus) 不 reset, 否则缓存被
  // 反复清空 → 大量词无标记 (2026-08-13 实测)。
  // 2026-08-18: reset 加 1s 合并 — 新版 WPS 内部 DocumentMgr 频繁 A↔B
  // 切换 (日志实测 5298 次 focus:switch, 出现 7 连发), 每次全清
  // (text+valid+gen++ 连 fallback) → 风暴期词无重排。首次切换仍立即
  // reset (真切窗首词保护不变); 1s 内的后续切换只重新采集不 reset —
  // 新文本 ~100ms 内送达覆盖旧文, 仅剩采集送达前的极短间隙有旧文风险
  // (风暴期无人工打字, 实际不可达)。
  if (pDocMgrFocus && pDocMgrFocus != pDocMgrPrevFocus) {
    ULONGLONG now = GetTickCount64();
    if (now - m_last_focus_reset_tick >= 1000) {
      m_last_focus_reset_tick = now;
      // 顺序: 先 spawn reset 线程 (立即发送, <5ms), 再发起采集请求
      // (TSF 异步编辑会话排队执行, 到达在 reset 之后) → 服务端先清旧再收新。
      std::thread([](weasel::Client* c) { c->ResetContext("focus:switch"); },
                  &m_client).detach();
      _OnContextReset();  // 清去抖标记, 新窗口首次采集强制重发
    }
    com_ptr<ITfContext> pContext;
    if (pDocMgrFocus->GetTop(&pContext) == S_OK && pContext)
      _RequestContextText(pContext);
    // 架构调研 A 探针 (实验): 焦点切换时枚举全部 context 试读, 仅日志
    if (pContext)
      _ProbeAllContexts(pContext);
  }

  com_ptr<ITfDocumentMgr> pCandidateListDocumentMgr;
  com_ptr<ITfContext> pTfContext = _GetUIContextDocument();
  if ((nullptr != pTfContext) &&
      SUCCEEDED(pTfContext->GetDocumentMgr(&pCandidateListDocumentMgr))) {
    if (pCandidateListDocumentMgr != pDocMgrFocus) {
      _HideUI();
    } else {
      _ShowUI();
    }
  }

  return S_OK;
}

STDAPI WeaselTSF::OnPushContext(ITfContext* pContext) {
  return S_OK;
}

STDAPI WeaselTSF::OnPopContext(ITfContext* pContext) {
  return S_OK;
}

BOOL WeaselTSF::_InitThreadMgrEventSink() {
  ITfSource* pSource;
  if (_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSource) != S_OK)
    return FALSE;
  if (pSource->AdviseSink(IID_ITfThreadMgrEventSink,
                          (ITfThreadMgrEventSink*)this,
                          &_dwThreadMgrEventSinkCookie) != S_OK) {
    _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    pSource->Release();
    return FALSE;
  }
  pSource->Release();
  return TRUE;
}

void WeaselTSF::_UninitThreadMgrEventSink() {
  ITfSource* pSource;
  if (_dwThreadMgrEventSinkCookie == TF_INVALID_COOKIE)
    return;
  if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSource))) {
    pSource->UnadviseSink(_dwThreadMgrEventSinkCookie);
    pSource->Release();
  }
  _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
}
