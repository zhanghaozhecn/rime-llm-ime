#include "stdafx.h"
#include "WeaselTSF.h"

static BOOL IsRangeCovered(TfEditCookie ec,
                           ITfRange* pRangeTest,
                           ITfRange* pRangeCover) {
  LONG lResult;

  if (pRangeCover->CompareStart(ec, pRangeTest, TF_ANCHOR_START, &lResult) !=
          S_OK ||
      lResult > 0)
    return FALSE;
  if (pRangeCover->CompareEnd(ec, pRangeTest, TF_ANCHOR_END, &lResult) !=
          S_OK ||
      lResult < 0)
    return FALSE;
  return TRUE;
}

STDAPI WeaselTSF::OnEndEdit(ITfContext* pContext,
                            TfEditCookie ecReadOnly,
                            ITfEditRecord* pEditRecord) {
  TSFDbgLog(L"OnEndEdit enter");
  BOOL fSelectionChanged;
  IEnumTfRanges* pEnumTextChanges;
  ITfRange* pRange;

  /* did the selection change? */
  if (pEditRecord->GetSelectionStatus(&fSelectionChanged) == S_OK &&
      fSelectionChanged) {
    if (_IsComposing()) {
      /* if the caret moves out of composition range, stop the composition */
      TF_SELECTION tfSelection;
      ULONG cFetched;

      if (pContext->GetSelection(ecReadOnly, TF_DEFAULT_SELECTION, 1,
                                 &tfSelection, &cFetched) == S_OK &&
          cFetched == 1) {
        ITfRange* pRangeComposition;
        if (_pComposition->GetRange(&pRangeComposition) == S_OK) {
          if (!IsRangeCovered(ecReadOnly, tfSelection.range, pRangeComposition))
            _EndComposition(pContext, true);
          pRangeComposition->Release();
        }
      }
    } else {
      // rime-llm-ime: 非 composing 时光标移动 (鼠标点击/导航键/方向键) →
      // 光标前文本已变, 立即重新采集。TSF 的 EditRecord selection status
      // 天然感知鼠标操作 (插件版 lua 做不到), 采集幂等 (相同文本不重发)。
      // 注意 composing 期间不采集: selection 在 composition 内,
      // 文档起点→光标会包含 composition 插入文本, 污染上文。
      TSFDbgLog(L"OnEndEdit selection-changed, refresh ctx");
      _RequestContextText(pContext);
    }
  }

  /* text modification? */
  if (pEditRecord->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT, NULL, 0,
                                             &pEnumTextChanges) == S_OK) {
    if (pEnumTextChanges->Next(1, &pRange, NULL) == S_OK) {
      pRange->Release();
      // 文档文本变化 (上屏/退格/删除/粘贴): 光标前文本已变, 异步刷新
      // LLM 上文采集。退格时按键采集拿到的是退格前文本, 文档变化后
      // 不刷新会一直用旧上文 (或 ResetContext 后无文本 → 不推理);
      // 去抖在 _OnContextTextReady 内 (300ms 合并 + 相同不重发)。
      _RequestContextText(pContext);
      // 架构调研 A 探针 (实验): 文本变化时也枚举全部 context 试读
      _ProbeAllContexts(pContext);
    }
    pEnumTextChanges->Release();
  }
  return S_OK;
}

STDAPI WeaselTSF::OnLayoutChange(ITfContext* pContext,
                                 TfLayoutCode lcode,
                                 ITfContextView* pContextView) {
  if (!_IsComposing())
    return S_OK;

  if (pContext != _pTextEditSinkContext)
    return S_OK;

  if (lcode == TF_LC_CHANGE)
    _UpdateCompositionWindow(pContext);
  return S_OK;
}

BOOL WeaselTSF::_InitTextEditSink(com_ptr<ITfDocumentMgr> pDocMgr) {
  com_ptr<ITfSource> pSource;
  BOOL fRet;

  /* clear out any previous sink first */
  if (_dwTextEditSinkCookie != TF_INVALID_COOKIE) {
    _pTextEditSinkContext->QueryInterface(&pSource);
    if (pSource != nullptr) {
      pSource->UnadviseSink(_dwTextEditSinkCookie);
      pSource->UnadviseSink(_dwTextLayoutSinkCookie);
    }
    _pTextEditSinkContext = nullptr;
    _dwTextEditSinkCookie = TF_INVALID_COOKIE;
  }
  if (pDocMgr == NULL)
    return TRUE;

  if (pDocMgr->GetTop(&_pTextEditSinkContext) != S_OK)
    return FALSE;

  if (_pTextEditSinkContext == NULL)
    return TRUE;

  fRet = FALSE;

  pSource.Release();

  if (_pTextEditSinkContext->QueryInterface(IID_ITfSource, (void**)&pSource) ==
      S_OK) {
    if (pSource->AdviseSink(IID_ITfTextEditSink, (ITfTextEditSink*)this,
                            &_dwTextEditSinkCookie) == S_OK)
      fRet = TRUE;
    else
      _dwTextEditSinkCookie = TF_INVALID_COOKIE;
    if (pSource->AdviseSink(IID_ITfTextLayoutSink, (ITfTextLayoutSink*)this,
                            &_dwTextLayoutSinkCookie) == S_OK) {
      fRet = TRUE;
    } else
      _dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
  }
  if (fRet == FALSE) {
    _pTextEditSinkContext = nullptr;
  }

  return fRet;
}
