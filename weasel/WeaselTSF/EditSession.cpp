#include "stdafx.h"
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "ResponseParser.h"

STDAPI WeaselTSF::DoEditSession(TfEditCookie ec) {
  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style());

  // 延迟计时（WEASEL_TSF_TIMING=1 才生效）：入口 t_sess、响应读完 t_ipc、
  // _UpdateUI 返回 t_ui（候选面板在 Refresh 内同步 RedrawWindow /
  // UpdateLayeredWindow 完成）；total = 按键回调入口→面板绘制完成
  LARGE_INTEGER t_sess, t_ipc, t_ui;
  QueryPerformanceCounter(&t_sess);

  bool ok = m_client.GetResponseData(std::ref(parser));
  QueryPerformanceCounter(&t_ipc);

  _UpdateLanguageBar(_status);

  if (ok) {
    if (!commit.empty()) {
      // For auto-selecting, commit and preedit can both exist.
      // Commit and close the original composition first.
      if (!_IsComposing()) {
        _StartComposition(_pEditSessionContext,
                          _fCUASWorkaroundEnabled && !config.inline_preedit);
      }
      _InsertText(_pEditSessionContext, commit);
      _EndComposition(_pEditSessionContext, false);
      _committed = TRUE;
    } else {
      _committed = FALSE;
    }
    if (_status.composing && !_IsComposing()) {
      _StartComposition(_pEditSessionContext,
                        _fCUASWorkaroundEnabled && !config.inline_preedit);
    } else if (!_status.composing && _IsComposing()) {
      _EndComposition(_pEditSessionContext, true);
    }
    if (_IsComposing() && config.inline_preedit) {
      _ShowInlinePreedit(_pEditSessionContext, context);
    }
    _UpdateCompositionWindow(_pEditSessionContext);
  }

  _UpdateUI(*context, _status);
  QueryPerformanceCounter(&t_ui);
  TSFTimingFinishSession((unsigned long long)t_sess.QuadPart,
                        (unsigned long long)t_ipc.QuadPart,
                        (unsigned long long)t_ui.QuadPart);

  return TRUE;
}
