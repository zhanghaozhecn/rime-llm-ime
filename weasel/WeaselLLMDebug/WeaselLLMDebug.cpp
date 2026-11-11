// WeaselLLMDebug.cpp — LLM 实时调试窗口（源码版；托盘菜单"LLM 调试窗口"启动）
//
// 数据源：命名管道 \\.\pipe\rime_llm_live —— librime llm_filter 在方案
// llm_rerank/debug_fusion: true 时推送的行协议（与插件版 rime_llm.dll 同名
// 同协议；插件版查看器为 rime-llm-rerank 仓库 debug\llm_debug_view.ps1，
// 行语义注释两边逐字一致）：
//   V|rime-llm-live|1|source|<日期>                  连接后服务端立即推
//   L|HH:MM:SS|input|cache|src|ctx|tick|ms|ready|n   每次触发评分的调用头
//                                                    （ctx=完整上文；HIT 时 ms=0）
//   C|eng|word|s1|ce|fb|u1|fused|full|cerank|fin|win 逐候选按最终序（上限 20）：
//                                                    eng=引擎原序 s1=层1分
//                                                    ce=CE 分（空=无分）
//                                                    fb=β·log1p(eff) u1=log(p1)
//                                                    full=两码整句满长
//                                                    fin=最终位置
//                                                    win=1 层2窗内/0 尾窗透传
//   S|HH:MM:SS|reason|input|src                     跳过原因
//   E|HH:MM:SS|word|eff0|eff1|tick0|tick1           commit 落账
//   R|HH:MM:SS|reason                               上文重置
//
// 结构（仿 WeaselLLMSetup）：cl 直编纯 Win32 + system DPI 感知（ts() 全覆盖，
// 核对钩子 WEASEL_LLM_DEBUG_DPI）+ WM_CTLCOLORSTATIC 白底。管道读取在独立
// 线程，跨线程只走 mutex 保护的行队列，UI 线程 WM_TIMER 100ms 排水——
// 不碰跨线程控件访问。
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- 控件 ID ----
#define IDC_STATUS 2001
#define IDC_INFO   2002
#define IDC_CTX    2003
#define IDC_PAUSE  2004
#define IDC_TOP    2005
#define IDC_CLEAR  2006
#define IDC_GRID   2007
#define IDC_HIST   2008
#define IDC_EVT    2009
#define IDC_FOOT   2010
#define TIMER_DRAIN 1

static const wchar_t *kPipeName = L"\\\\.\\pipe\\rime_llm_live";

// ---- DPI：清单 system 感知，设计像素（96 基准）→ 设备像素 ----
static int ts(int v) {
  static int dpi = -1;
  if (dpi < 0) {
    dpi = 0;
    wchar_t env[16] = L"";
    DWORD n = GetEnvironmentVariableW(L"WEASEL_LLM_DEBUG_DPI", env, 16);
    if (n > 0 && n < 16) dpi = _wtoi(env);
    if (dpi <= 0) {
      HDC dc = GetDC(NULL);
      dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
      if (dc) ReleaseDC(NULL, dc);
      if (dpi <= 0) dpi = 96;
    }
  }
  return MulDiv(v, dpi, 96);
}

// ---- 小工具 ----
static std::wstring u8w(const std::string &s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
  std::wstring r(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &r[0], n);
  return r;
}

// 按 '|' 切分（保留空字段——ce 空串=无分是协议约定）
static void split8(const std::string &s, std::vector<std::string> &out) {
  out.clear();
  size_t b = 0;
  for (;;) {
    size_t p = s.find('|', b);
    if (p == std::string::npos) {
      out.push_back(s.substr(b));
      return;
    }
    out.push_back(s.substr(b, p - b));
    b = p + 1;
  }
}

// ---- 数据模型 ----
struct Row {
  std::wstring eng, word, s1, ce, fb, u1, fused, full, cerank, fin, win;
  bool win0 = false;  // 尾窗透传（灰字）
};
struct Inv {
  std::wstring t, input, cache, src, ctx, tick, ms, ready, n;
  std::vector<Row> rows;
};
static std::deque<Inv> g_invs;          // 最近 50 次调用（新的在尾）
static std::deque<std::wstring> g_evts; // 最近 200 条事件（新的在头）
static int g_view = -1;                 // 当前展示下标（相对 g_invs 头）
static bool g_paused = false;
static std::wstring g_ver;

// ---- 管道读取线程 ----
static std::atomic<bool> g_stop{false};
static std::mutex g_qmu;
static std::deque<std::string> g_lines;  // 未处理行（UTF-8）
static std::atomic<bool> g_connected{false};

static void q_push(const std::string &l) {
  std::lock_guard<std::mutex> lk(g_qmu);
  g_lines.push_back(l);
}

static void reader_thread() {
  while (!g_stop.load()) {
    HANDLE p = CreateFileW(kPipeName, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    if (p == INVALID_HANDLE_VALUE) {
      Sleep(1000);  // 管道不存在（debug_fusion 未开/服务未跑）——慢重试
      continue;
    }
    g_connected.store(true);
    q_push("::CONNECTED");
    char buf[4096];
    std::string pend;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    bool fin = false;
    while (!fin && !g_stop.load()) {
      OVERLAPPED ov = {};
      ov.hEvent = ev;
      ResetEvent(ev);
      DWORD got = 0;
      BOOL okb = ReadFile(p, buf, sizeof(buf), NULL, &ov);
      if (okb || GetLastError() == ERROR_IO_PENDING) {
        DWORD wr = WaitForSingleObject(ev, 1000);
        if (wr == WAIT_OBJECT_0) {
          if (!GetOverlappedResult(p, &ov, &got, FALSE) || got == 0)
            fin = true;  // 对端关闭
        } else {
          // 1s 超时：必须先取消本次在途读再回圈——任一时刻至多一个在途
          // 读。旧写法超时后不取消、下轮又挂一个新读：两个在途读共用一
          // 个事件，下一批数据完成的是**旧读**（字节被丢弃），而
          // GetOverlappedResult 查的是**新 OVERLAPPED** → INCOMPLETE 被
          // 误判为断连 → 整批 L/C 丢失后重连。打字节奏（词间停顿 >1s）
          // 每词必踩：上文/候选表/历史全冻结，只剩重连后到达的 E/R 事件
          // 行（2026-11-11 真机 + 本地 3s 间隔 4 轮假服务端复现）。
          CancelIo(p);
          if (GetOverlappedResult(p, &ov, &got, TRUE) && got > 0) {
            // 竞态窗口内数据已到（取消前读已完成）——照常处理这批字节
            pend.append(buf, buf + got);
            for (;;) {
              size_t nl = pend.find('\n');
              if (nl == std::string::npos) break;
              std::string line = pend.substr(0, nl);
              pend.erase(0, nl + 1);
              if (!line.empty() && line.back() == '\r') line.pop_back();
              if (!line.empty()) q_push(line);
            }
          }
          continue;
        }
      } else {
        fin = true;
      }
      if (!fin && got > 0) {
        pend.append(buf, buf + got);
        for (;;) {
          size_t nl = pend.find('\n');
          if (nl == std::string::npos) break;
          std::string line = pend.substr(0, nl);
          pend.erase(0, nl + 1);
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (!line.empty()) q_push(line);
        }
      }
    }
    // 收尾：取消在途读 + 关句柄
    CancelIo(p);
    if (ev) CloseHandle(ev);
    CloseHandle(p);
    g_connected.store(false);
    q_push("::DISCONNECTED");
    if (!g_stop.load()) Sleep(1000);  // 断开后 1s 重连
  }
}

// ---- UI 状态 ----
static HWND g_hwnd = NULL;
static HFONT g_font = NULL, g_font_bold = NULL, g_font_hint = NULL;
static HWND h_status, h_info, h_ctx, h_grid, h_hist, h_evt, h_foot;

static void set_status(const wchar_t *text, bool ok) {
  SetWindowTextW(h_status, text);
  // 颜色用 STATIC 所画文字色不可直接设——用前缀符号表达状态即可（●/○）
  (void)ok;
}

static void fill_grid(const Inv &inv) {
  ListView_DeleteAllItems(h_grid);
  int i = 0;
  for (auto &r : inv.rows) {
    LVITEMW it = {};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    int idx = i + 1;
    wchar_t nb[16];
    swprintf(nb, 16, L"%d", idx);
    it.pszText = nb;
    it.lParam = (LPARAM)(r.win0 ? 1 : 0);
    int at = ListView_InsertItem(h_grid, &it);
    wchar_t cerank_fin[48];
    swprintf(cerank_fin, 48, L"%s→%s", r.cerank.c_str(), r.fin.c_str());
    struct { int col; const wchar_t *t; } cells[] = {
        {1, r.word.c_str()},
        {2, r.ce.empty() ? L"·" : r.ce.c_str()},
        {3, r.s1.c_str()},
        {4, r.fb.c_str()},
        {5, r.u1.c_str()},
        {6, r.fused.c_str()},
        {7, r.full.c_str()},
        {8, cerank_fin},
        {9, r.win.c_str()},
    };
    for (auto &c : cells)
      ListView_SetItemText(h_grid, at, c.col, (LPWSTR)c.t);
    i++;
  }
}

static void update_current() {
  if (g_view < 0 || g_view >= (int)g_invs.size()) {
    SetWindowTextW(h_info, L"—");
    SetWindowTextW(h_ctx, L"上文：—");
    ListView_DeleteAllItems(h_grid);
    return;
  }
  const Inv &inv = g_invs[g_view];
  wchar_t line[512];
  swprintf(line, 512,
           L"%s  输入 %s   缓存 %s   %sms   tick %s   就绪 %s   n=%s   来源 %s",
           inv.t.c_str(), inv.input.c_str(), inv.cache.c_str(), inv.ms.c_str(),
           inv.tick.c_str(), inv.ready.c_str(), inv.n.c_str(), inv.src.c_str());
  SetWindowTextW(h_info, line);
  SetWindowTextW(h_ctx,
                 (std::wstring(L"上文：") +
                  (inv.ctx.empty() ? L"（空）" : inv.ctx)).c_str());
  fill_grid(inv);
}

static void evt_add(const std::wstring &text) {
  g_evts.push_front(text);
  while (g_evts.size() > 200) g_evts.pop_back();
  if (!g_paused) {
    SendMessageW(h_evt, LB_INSERTSTRING, 0, (LPARAM)text.c_str());
    int cnt = (int)SendMessageW(h_evt, LB_GETCOUNT, 0, 0);
    if (cnt > 200) SendMessageW(h_evt, LB_DELETESTRING, cnt - 1, 0);
  }
}

static void hist_refresh_item() {
  // 最新历史条目补前 3 个词（首行 C 到达时）
  if (g_invs.empty()) return;
  Inv &inv = g_invs.back();
  if (inv.rows.size() > 3) return;
  std::wstring words;
  for (size_t i = 0; i < inv.rows.size() && i < 3; i++) {
    if (i) words += L",";
    words += inv.rows[i].word;
  }
  std::wstring txt = inv.t + L" " + inv.input + L" " + words;
  int cnt = (int)SendMessageW(h_hist, LB_GETCOUNT, 0, 0);
  if (cnt > 0) SendMessageW(h_hist, LB_DELETESTRING, cnt - 1, 0);
  SendMessageW(h_hist, LB_ADDSTRING, 0, (LPARAM)txt.c_str());
}

// ---- 协议行处理（UI 线程）----
static void handle_line(const std::string &line) {
  if (line == "::CONNECTED") {
    g_connected.store(true);
    if (!g_paused)
      set_status((L"● 已连接" + (g_ver.empty() ? L"" : L" · " + g_ver)).c_str(),
                 true);
    return;
  }
  if (line == "::DISCONNECTED") {
    g_connected.store(false);
    if (!g_paused)
      set_status(L"○ 连接断开，1 秒后重连…（管道不存在 = debug_fusion 未开"
                 L"或小狼毫服务未运行）",
                 false);
    return;
  }
  std::vector<std::string> f;
  split8(line, f);
  if (f.empty()) return;
  if (f[0] == "V" && f.size() >= 5 && f[1] == "rime-llm-live") {
    g_ver = u8w(f[2]) + L" · " + u8w(f[3]) + L" 版 · " + u8w(f[4]);
    if (g_connected.load() && !g_paused)
      set_status((L"● 已连接 · 协议 v" + g_ver).c_str(), true);
    return;
  }
  if (f[0] == "L" && f.size() >= 10) {
    Inv inv;
    inv.t = u8w(f[1]);
    inv.input = u8w(f[2]);
    inv.cache = u8w(f[3]);
    inv.src = u8w(f[4]);
    inv.ctx = u8w(f[5]);
    inv.tick = u8w(f[6]);
    inv.ms = u8w(f[7]);
    inv.ready = u8w(f[8]);
    inv.n = u8w(f[9]);
    g_invs.push_back(std::move(inv));
    while (g_invs.size() > 50) {
      g_invs.pop_front();
      if (g_view > 0) g_view--;
    }
    g_view = (int)g_invs.size() - 1;
    if (!g_paused) {
      Inv &back = g_invs.back();
      SendMessageW(h_hist, LB_ADDSTRING, 0,
                   (LPARAM)(back.t + L" " + back.input).c_str());
      int cnt = (int)SendMessageW(h_hist, LB_GETCOUNT, 0, 0);
      while (cnt > 50) {
        SendMessageW(h_hist, LB_DELETESTRING, 0, 0);
        cnt--;
      }
      update_current();
    }
    return;
  }
  if (f[0] == "C" && f.size() >= 12 && !g_invs.empty()) {
    Row r;
    r.eng = u8w(f[1]);
    r.word = u8w(f[2]);
    r.s1 = u8w(f[3]);
    r.ce = u8w(f[4]);
    r.fb = u8w(f[5]);
    r.u1 = u8w(f[6]);
    r.fused = u8w(f[7]);
    r.full = u8w(f[8]);
    r.cerank = u8w(f[9]);
    r.fin = u8w(f[10]);
    r.win = u8w(f[11]);
    r.win0 = (r.win == L"0");
    g_invs.back().rows.push_back(std::move(r));
    if (!g_paused) {
      if (g_view == (int)g_invs.size() - 1) update_current();
      hist_refresh_item();
    }
    return;
  }
  wchar_t tmp[512];
  if (f[0] == "S" && f.size() >= 3) {
    swprintf(tmp, 512, L"%s 跳过 %s（输入 %s 来源 %s）", u8w(f[1]).c_str(),
             u8w(f[2]).c_str(), f.size() > 3 ? u8w(f[3]).c_str() : L"",
             f.size() > 4 ? u8w(f[4]).c_str() : L"-");
    evt_add(tmp);
    return;
  }
  if (f[0] == "E" && f.size() >= 7) {
    swprintf(tmp, 512, L"%s 上屏 %s  eff %s→%s  tick %s→%s", u8w(f[1]).c_str(),
             u8w(f[2]).c_str(), u8w(f[3]).c_str(), u8w(f[4]).c_str(),
             u8w(f[5]).c_str(), u8w(f[6]).c_str());
    evt_add(tmp);
    return;
  }
  if (f[0] == "R" && f.size() >= 3) {
    swprintf(tmp, 512, L"%s 重置 %s", u8w(f[1]).c_str(), u8w(f[2]).c_str());
    evt_add(tmp);
    return;
  }
}

// ---- UI 构建（96 DPI 设计值）----
static HWND mk_ctl(const wchar_t *cls, const wchar_t *text, DWORD style, int x,
                   int y, int w, int h, int id, HFONT f) {
  HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, ts(x), ts(y),
                         ts(w), ts(h), g_hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
  return c;
}

static void make_ui() {
  g_font = CreateFontW(-ts(13), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                       L"Microsoft YaHei UI");
  g_font_bold = CreateFontW(-ts(13), 0, 0, 0, FW_BOLD, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Microsoft YaHei UI");
  g_font_hint = CreateFontW(-ts(11), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Microsoft YaHei UI");

  h_status = mk_ctl(L"STATIC", L"等待连接…（在方案开启 llm_rerank/debug_fusion"
                                L" 并重新部署，然后打字）",
                    SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 8, 520, 22,
                    IDC_STATUS, g_font);
  h_info = mk_ctl(L"STATIC", L"—", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 34,
                  764, 20, IDC_INFO, g_font_bold);
  h_ctx = mk_ctl(L"STATIC", L"上文：—", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8,
                 58, 764, 42, IDC_CTX, g_font);
  mk_ctl(L"BUTTON", L"暂停", BS_PUSHBUTTON, 536, 6, 64, 24, IDC_PAUSE, g_font);
  mk_ctl(L"BUTTON", L"置顶", BS_AUTOCHECKBOX, 608, 8, 52, 22, IDC_TOP, g_font);
  mk_ctl(L"BUTTON", L"清空", BS_PUSHBUTTON, 668, 6, 52, 24, IDC_CLEAR, g_font);

  h_grid = mk_ctl(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                      LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                  8, 104, 548, 298, IDC_GRID, g_font);
  ListView_SetExtendedListViewStyle(
      h_grid, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
  struct { const wchar_t *t; int w; } cols[] = {
      {L"#", 24}, {L"词", 58}, {L"CE", 62}, {L"层1", 56}, {L"频+", 56},
      {L"u1+", 56}, {L"融合", 62}, {L"满", 34}, {L"序变", 44}, {L"窗内", 44}};
  LVCOLUMNW col = {};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  for (int i = 0; i < 10; i++) {
    col.pszText = (LPWSTR)cols[i].t;
    col.cx = ts(cols[i].w);
    col.iSubItem = i;
    ListView_InsertColumn(h_grid, i, &col);
  }

  h_hist = mk_ctl(L"LISTBOX", L"",
                  WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL |
                      LBS_NOINTEGRALHEIGHT,
                  564, 104, 208, 298, IDC_HIST, g_font);
  h_evt = mk_ctl(L"LISTBOX", L"",
                 WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL |
                     LBS_NOINTEGRALHEIGHT,
                 8, 410, 764, 116, IDC_EVT, g_font);
  h_foot = mk_ctl(L"STATIC",
                  L"数据源 \\\\.\\pipe\\rime_llm_live · 开关 = 方案 "
                  L"llm_rerank/debug_fusion（改后重新部署）· 窗内=0 行 = "
                  L"层1 尾窗透传（灰字）",
                  SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 532, 764, 16, IDC_FOOT,
                  g_font_hint);
}

// 尾窗透传行灰字（NM_CUSTOMDRAW）
static LRESULT on_grid_customdraw(NMLVCUSTOMDRAW *cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
      if (cd->nmcd.lItemlParam)
        cd->clrText = RGB(150, 150, 150);
      return CDRF_DODEFAULT;
  }
  return CDRF_DODEFAULT;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_hwnd = h;
      make_ui();
      RECT cr = {0, 0, ts(780), ts(554)};
      AdjustWindowRect(&cr, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE);
      SetWindowPos(h, NULL, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      std::thread(reader_thread).detach();
      SetTimer(h, TIMER_DRAIN, 100, NULL);
      return 0;
    }
    case WM_TIMER:
      if (wp == TIMER_DRAIN) {
        for (;;) {
          std::string line;
          {
            std::lock_guard<std::mutex> lk(g_qmu);
            if (g_lines.empty()) break;
            line = std::move(g_lines.front());
            g_lines.pop_front();
          }
          handle_line(line);
        }
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_PAUSE:
          g_paused = !g_paused;
          SetWindowTextW(GetDlgItem(h, IDC_PAUSE), g_paused ? L"继续" : L"暂停");
          if (!g_paused) {
            // 恢复：重建事件与历史列表（暂停期间两者都在变）+ 当前视图
            SendMessageW(h_evt, LB_RESETCONTENT, 0, 0);
            for (auto &e : g_evts)
              SendMessageW(h_evt, LB_ADDSTRING, 0, (LPARAM)e.c_str());
            SendMessageW(h_hist, LB_RESETCONTENT, 0, 0);
            for (auto &inv : g_invs) {
              std::wstring words;
              for (size_t i = 0; i < inv.rows.size() && i < 3; i++) {
                if (i) words += L",";
                words += inv.rows[i].word;
              }
              SendMessageW(h_hist, LB_ADDSTRING, 0,
                           (LPARAM)(inv.t + L" " + inv.input + L" " + words)
                               .c_str());
            }
            update_current();
          }
          return 0;
        case IDC_TOP:
          if (SendMessageW(GetDlgItem(h, IDC_TOP), BM_GETCHECK, 0, 0))
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
          else
            SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
          return 0;
        case IDC_CLEAR:
          g_invs.clear();
          g_evts.clear();
          g_view = -1;
          SendMessageW(h_hist, LB_RESETCONTENT, 0, 0);
          SendMessageW(h_evt, LB_RESETCONTENT, 0, 0);
          update_current();
          return 0;
        case IDCANCEL:
          DestroyWindow(h);
          return 0;
      }
      if (LOWORD(wp) == IDC_HIST && HIWORD(wp) == LBN_SELCHANGE) {
        int sel = (int)SendMessageW(h_hist, LB_GETCURSEL, 0, 0);
        if (sel >= 0) {
          int cnt = (int)SendMessageW(h_hist, LB_GETCOUNT, 0, 0);
          g_view = (int)g_invs.size() - cnt + sel;
          if (g_view < 0) g_view = 0;
          if (g_view >= (int)g_invs.size())
            g_view = (int)g_invs.size() - 1;
          update_current();
        }
        return 0;
      }
      break;
    case WM_NOTIFY: {
      NMHDR *nm = (NMHDR *)lp;
      if (nm->idFrom == IDC_GRID && nm->code == NM_CUSTOMDRAW)
        return on_grid_customdraw((NMLVCUSTOMDRAW *)lp);
      break;
    }
    case WM_CTLCOLORSTATIC:
      // 白底：STATIC 默认用 COLOR_BTNFACE 擦底（灰条）——透明 + 返回白刷
      SetBkMode((HDC)wp, TRANSPARENT);
      return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_DESTROY:
      KillTimer(h, TIMER_DRAIN);
      g_stop.store(true);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
  INITCOMMONCONTROLSEX icc = {sizeof(icc),
                              ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  WNDCLASSW wc = {};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
  wc.lpszClassName = L"WeaselLLMDebug";
  RegisterClassW(&wc);
  HWND h = CreateWindowExW(0, L"WeaselLLMDebug",
                           L"LLM 实时调试 — rime-llm-ime（源码版）",
                           WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX &
                               ~WS_THICKFRAME,
                           CW_USEDEFAULT, CW_USEDEFAULT, ts(780), ts(554),
                           NULL, NULL, inst, NULL);
  ShowWindow(h, show);
  UpdateWindow(h);
  MSG m;
  HACCEL acc = NULL;  // 无加速键；IsDialogMessage 提供 Tab/ESC 导航
  while (GetMessageW(&m, NULL, 0, 0) > 0) {
    if (!IsDialogMessageW(h, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  (void)acc;
  return (int)m.wParam;
}
