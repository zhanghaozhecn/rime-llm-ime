// WeaselLLMDebug.cpp — LLM 实时调试窗口（源码版；托盘菜单"LLM 调试窗口"启动）
//
// 数据源：命名管道 \\.\pipe\rime_llm_live —— librime llm_filter 在方案
// llm_rerank/debug_fusion: true 时推送的行协议（与插件版 rime_llm.dll 同名
// 同协议；插件版查看器为 rime-llm-rerank 仓库 debug\llm_debug_view.ps1，
// 行语义注释两边逐字一致）：
//   V|rime-llm-live|1|source|<日期>                  连接后服务端立即推
//                                                    （协议握手保留；版本/日期徽章
//                                                    自 2026-11-16 不再显示）
//   L|HH:MM:SS|input|cache|src|ctx|tick|ms|ready|n|prep|ctx_used
//                                                    每次触发评分的调用头
//                                                    （ctx=完整去空白上文；prep=KV 预解
//                                                    命中；n=候选数——协议保留，
//                                                    查看器不显示；v4 尾字段
//                                                    ctx_used=推理用尾部——查看器
//                                                    灰显其余前缀；n_seen 字段
//                                                    2026-11-17 删除（不参与打分））
//   C|eng|word|s1|ce|eff|uni|fused|full|cerank|fin|win|ce1|ce2|ce3|fb|u1|pf|sdelta
//                                                    pf=旧修正分 λ（仅扣分, v5）
//                                                    sdelta=有符号词码对分数
//                                                    （v6, +200/+100/-5/0）
//                                                    逐候选按最终序（上限 20）：
//                                                    s1=层1分(=fb+u1)
//                                                    ce=加权 CE（空=无分）
//                                                    eff=衰减计数 uni=累计次数
//                                                    fused=融合分（无 CE 时=fb+u1）
//                                                    full=满长 win=1窗内/0尾窗
//                                                    ce1/2/3=分位原始CE(未乘权重)
//                                                    fb=β·log1p(eff) u1=ln(uni+1)
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
#include <richedit.h>
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
#define IDC_STREAM 2008  // 合并流：推理项 + 日志（2026-11-17 定案——原 IDC_HIST/
                         // IDC_EVT 两块合一；◆ 行可点开详情、灰字日志行不可点开）
#define IDC_FOOT   2010
#define IDC_TYPE   2011  // 打字测试框（在此输入即走本机 IME，2026-11-16）
#define IDC_TYPELBL 2012
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
  // eng/s1/cerank/fin 协议保留但窗口不再单列展示（行序=最终序，见 # 列）
  std::wstring eng, word, s1, ce, eff, uni, fused, full, cerank, fin, win;
  std::wstring ce1, ce2, ce3;  // CE 三分量（未乘权重）
  std::wstring fb, u1;         // 衰减词频分 β·log1p(eff) / 累计词频分 log p1
  std::wstring pf;             // v5: 旧修正分 λ（负分幅度, 0=未命中）
  std::wstring sdelta;         // v6: 有符号词码对分数（+200/+100/-5/0）
  bool win0 = false;  // 尾窗透传（灰字）
};
struct Inv {
  std::wstring t, input, cache, src, ctx, tick, ms, ready, prep, ctxused;
  std::vector<Row> rows;
  int seq = 0;   // 推理序号：合并流行经它关联回本调用（2026-11-17）
};
static std::deque<Inv> g_invs;          // 最近 50 次调用（新的在尾）
// 合并流（2026-11-17 用户定案：原「历史」〔推理项列表，可点开详情〕与「事件」
// 〔纯日志〕两块合一）：按到达顺序排（新的在尾、跟随滚动到底），推理行 ◆
// 黑字可点开、日志行灰字不可点开——行为同前只是合到一起
struct StreamEnt {
  bool inv;            // true = 推理项行（可点开详情）
  int seq;             // inv 行 = 所属调用序号；日志行 = 0
  std::wstring text;
};
static std::deque<StreamEnt> g_stream;  // 最近 200 行
static int g_inv_seq = 0;
static int g_view = -1;                 // 当前展示下标（相对 g_invs 头）
static bool g_paused = false;

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
static HFONT g_font = NULL, g_font_bold = NULL, g_font_hint = NULL,
              g_font_mono = NULL;
static HWND h_status, h_info, h_ctx, h_grid, h_stream, h_foot;
static bool g_suppress_sel = false;  // 程序化拨高亮时抑制 LBN_SELCHANGE 逻辑
static bool g_ctx_is_rich = false;  // 上文行 = RichEdit（false = 退回 STATIC，不灰显）

static void set_status(const wchar_t *text, bool ok) {
  SetWindowTextW(h_status, text);
  // 颜色用 STATIC 所画文字色不可直接设——用前缀符号表达状态即可（●/○）
  (void)ok;
}

// ---- 分数格 = "得分=计算过程"（2026-11-11 用户定案；底部公式行随之删除）----
// 数值字段解析：生产端 live_num/live_fmt3 对 无分/NaN 发空串 → 空即缺失
static bool num_of(const std::wstring &s, double &v) {
  if (s.empty()) return false;
  wchar_t *e = NULL;
  v = wcstod(s.c_str(), &e);
  if (e == s.c_str()) return false;
  return v == v;  // NaN → 缺失
}
static std::wstring f1(double v) {
  wchar_t b[32];
  swprintf(b, 32, L"%.1f", v);
  return b;
}
// 左补空格到指定宽（右对齐）：各数值小数位数一致时小数点即逐位对齐
static std::wstring padl(const std::wstring &s, int w) {
  if ((int)s.size() >= w) return s;
  return std::wstring((size_t)(w - (int)s.size()), L' ') + s;
}

// 修正分格（2026-11-17 v6：有符号词码对分数）：+200/+100 = 固定首选/
// 次选加分，-5 = pairfix 扣分，0.0 = 未命中；验算
// 总分 = CE + 衰减词频分 + 累计词频分 + 修正分。旧发送端无 sdelta 时
// 回退 v5 的 `-λ`/0.0 显示。
static std::wstring pf_proc(const Row &r, int wl) {
  double sd;
  if (num_of(r.sdelta, sd)) {
    if (sd > 0) return padl(L"+" + f1(sd), wl);
    return padl(f1(sd), wl);
  }
  double pf;
  if (!num_of(r.pf, pf)) return L"—";
  return padl(pf > 0 ? (L"-") + f1(pf) : f1(0.0), wl);
}

// 动态对齐（2026-11-11 用户定案）：本次调用内每个数值组向最宽者看齐。
// 每列分两组——分数（等号左）与计算过程里的量，分别量宽。
struct ColW {
  int ce_l, ce_c;  // CE 列：分数 / 三分量
  int fb_l, fb_e;  // 衰减词频分列：分数 / eff（2 位小数）
  int u1_l, uni;   // 累计词频列：分数 / 累计次数（整数）
  int fu_l;        // 总分列：纯分值（修正分已单列，2026-11-16）
  int pf_l;        // 修正分列：有符号 +200/-5/0.0
};
static void measure_widths(const Inv &inv, ColW &w) {
  w = {0, 0, 0, 0, 0, 0, 0, 0};
  auto mx = [](int a, int b) { return a > b ? a : b; };
  double v;
  for (auto &r : inv.rows) {
    if (num_of(r.ce, v)) w.ce_l = mx(w.ce_l, (int)f1(v).size());
    if (num_of(r.ce1, v)) w.ce_c = mx(w.ce_c, (int)f1(v).size());
    if (num_of(r.ce2, v)) w.ce_c = mx(w.ce_c, (int)f1(v).size());
    if (num_of(r.ce3, v)) w.ce_c = mx(w.ce_c, (int)f1(v).size());
    if (num_of(r.fb, v)) w.fb_l = mx(w.fb_l, (int)f1(v).size());
    if (num_of(r.eff, v)) w.fb_e = mx(w.fb_e, (int)f1(v).size());
    if (num_of(r.u1, v)) w.u1_l = mx(w.u1_l, (int)f1(v).size());
    if (num_of(r.uni, v)) {
      wchar_t b[16];
      swprintf(b, 16, L"%d", (int)v);
      w.uni = mx(w.uni, (int)wcslen(b));
    }
    if (num_of(r.fused, v)) w.fu_l = mx(w.fu_l, (int)f1(v).size());
    if (num_of(r.sdelta, v)) {
      int n = (int)f1(v).size() + (v > 0 ? 1 : 0);
      w.pf_l = mx(w.pf_l, n);
    } else if (num_of(r.pf, v)) {
      int n = v > 0 ? (int)(f1(v).size() + 1) : 3;  // -λ 带 ASCII 负号 / "0.0"
      w.pf_l = mx(w.pf_l, n);
    }
  }
}

// CE 格：加权 CE 与三分量。生产语义 = ce = -(Σ kCeW·分量)（分量是原始
// 交叉熵、正值；分值取负号进融合），故等号右为带负号的括号和。
// ce2/ce3 带 kCeW 权重前缀（1.10/0.60 = librime 标定内置常数，原样显示；
// 2026-11-14 随默认模型 0.5B 重标定，旧 1.05/0.45 为 0.8B v3 值）；
// ce1 权重恒 1 → 前缀不显示（2026-11-12 用户定案）；
// 分量缺（无分/未参与）的项整项省略。
static std::wstring ce_proc(const Row &r, int wl, int wc) {
  double ce;
  if (!num_of(r.ce, ce)) return L"—";
  std::wstring lead = padl(f1(ce), wl);
  std::wstring t = lead + L"=-(";
  bool any = false;
  const struct {
    const wchar_t *w;  // nullptr = 权重恒 1，不显前缀
    const std::wstring *raw;
  } t3[3] = {{nullptr, &r.ce1}, {L"1.10", &r.ce2}, {L"0.60", &r.ce3}};
  for (auto &x : t3) {
    double v;
    if (!num_of(*x.raw, v)) continue;
    if (any) t += L"+";
    if (x.w) {
      wchar_t b[48];
      swprintf(b, 48, L"%s·", x.w);
      t += b;
    }
    t += padl(f1(v), wc);
    any = true;
  }
  if (!any) return lead;  // 旧协议无分量 → 只显分数
  return t + L")";
}

// 衰减词频分格：β·log1p(eff)，β=14 内置常数
// （2026-11-14 随默认模型 0.5B 重标定，旧 β=12 为 0.8B v3 值）
static std::wstring fb_proc(const Row &r, int wl, int we) {
  double fb, eff;
  if (!num_of(r.fb, fb)) return L"—";
  std::wstring lead = padl(f1(fb), wl);
  if (!num_of(r.eff, eff)) return lead;
  return lead + L"=14·log1p(" + padl(f1(eff), we) + L")";
}

// 累计词频格：log1p(uni)（2026-11-11 公式简化，分母为窗内常数已去除；
// 2026-11-18 显示统一 log1p(n)）
static std::wstring u1_proc(const Row &r, int wl, int wu) {
  double u1, uni;
  if (!num_of(r.u1, u1)) return L"—";
  std::wstring lead = padl(f1(u1), wl);
  if (!num_of(r.uni, uni)) return lead;
  wchar_t ub[16], b[48];
  swprintf(ub, 16, L"%d", (int)uni);
  swprintf(b, 48, L"=log1p(%s)", padl(ub, wu).c_str());
  return lead + b;
}

// 总分格：融合分纯分值（三项过程各在自带格内，修正分见专属列；
// 尾窗透传行无融合分 → —）
static std::wstring fused_proc(const Row &r, int wl) {
  double fu;
  if (!num_of(r.fused, fu)) return L"—";
  return padl(f1(fu), wl);
}

static void fill_grid(const Inv &inv) {
  ListView_DeleteAllItems(h_grid);
  ColW w;
  measure_widths(inv, w);
  int i = 0;
  for (auto &r : inv.rows) {
    LVITEMW it = {};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = i;  // 必须显式给位次：缺省 0 = 每次插在最顶 → 整表倒序
    int idx = i + 1;
    wchar_t nb[16];
    swprintf(nb, 16, L"%d", idx);
    it.pszText = nb;
    it.lParam = (LPARAM)(r.win0 ? 1 : 0);
    int at = ListView_InsertItem(h_grid, &it);
    std::wstring fu_txt = fused_proc(r, w.fu_l);
    std::wstring ce_txt = ce_proc(r, w.ce_l, w.ce_c);
    std::wstring fb_txt = fb_proc(r, w.fb_l, w.fb_e);
    std::wstring u1_txt = u1_proc(r, w.u1_l, w.uni);
    std::wstring pf_txt = pf_proc(r, w.pf_l);
    struct { int col; const wchar_t *t; } cells[] = {
        {1, r.word.c_str()},
        {2, fu_txt.c_str()},
        {3, ce_txt.c_str()},
        {4, fb_txt.c_str()},
        {5, u1_txt.c_str()},
        {6, pf_txt.c_str()},
    };
    for (auto &c : cells)
      ListView_SetItemText(h_grid, at, c.col, (LPWSTR)c.t);
    i++;
  }
}

// 上文行灰显（协议 v4 ctx_used）：'上文：' 头 + 未用前缀灰（RGB 150）+
// 推理用尾部正常色；used 空/找不到/非 RichEdit → 全文正常色
static void ctx_render(const std::wstring &ctx, const std::wstring &used) {
  std::wstring full = L"上文：" + (ctx.empty() ? L"（空）" : ctx);
  SetWindowTextW(h_ctx, full.c_str());
  if (!g_ctx_is_rich) return;
  size_t pos = used.empty() ? std::wstring::npos : full.rfind(used);
  if (pos == std::wstring::npos || pos <= 3) return;  // 无前缀可灰
  CHARFORMAT2W cf = {};
  cf.cbSize = sizeof(cf);
  cf.dwMask = CFM_COLOR;
  CHARRANGE cr;
  cr.cpMin = 3;  // '上文：' 3 字符头保持正常色
  cr.cpMax = (LONG)pos;
  SendMessageW(h_ctx, EM_EXSETSEL, 0, (LPARAM)&cr);
  cf.crTextColor = RGB(150, 150, 150);
  SendMessageW(h_ctx, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
  cr.cpMin = (LONG)pos;
  cr.cpMax = -1;
  SendMessageW(h_ctx, EM_EXSETSEL, 0, (LPARAM)&cr);
  cf.crTextColor = RGB(0, 0, 0);
  SendMessageW(h_ctx, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
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
           L"%s  输入 %s   来源 %s   预解 %s   %sms   tick %s"
           L"   就绪 %s",
           inv.t.c_str(), inv.input.c_str(), inv.src.c_str(),
           inv.prep.empty() ? L"-" : inv.prep.c_str(), inv.ms.c_str(),
           inv.tick.c_str(), inv.ready.c_str());
  SetWindowTextW(h_info, line);
  ctx_render(inv.ctx, inv.ctxused);
  fill_grid(inv);
}

// ── 合并流（2026-11-17：历史+事件两块合一）────────────────────────────
// 推理行文本：◆ 时间 输入 词1,词2,词3（前 3 词；C 行到达时补齐）
static std::wstring inv_row_text(const Inv &inv) {
  std::wstring words;
  for (size_t i = 0; i < inv.rows.size() && i < 3; i++) {
    if (i) words += L",";
    words += inv.rows[i].word;
  }
  std::wstring t = L"◆ " + inv.t + L" " + inv.input;
  if (!words.empty()) t += L" " + words;
  return t;
}

// 追加一行（g_stream 是唯一事实源；未暂停时同步 ListBox 并跟随最新）
static void stream_add(bool inv, const std::wstring &text, int seq) {
  g_stream.push_back({inv, inv ? seq : 0, text});
  while (g_stream.size() > 200) g_stream.pop_front();
  if (g_paused) return;
  SendMessageW(h_stream, LB_ADDSTRING, 0, (LPARAM)text.c_str());
  int cnt = (int)SendMessageW(h_stream, LB_GETCOUNT, 0, 0);
  while (cnt > 200) {
    SendMessageW(h_stream, LB_DELETESTRING, 0, 0);
    cnt--;
  }
  // 高亮只管推理行（= 详情区所示那条）；日志行只把视图滚到尾部
  if (inv) {
    g_suppress_sel = true;
    SendMessageW(h_stream, LB_SETCURSEL, cnt - 1, 0);
    g_suppress_sel = false;
  } else {
    SendMessageW(h_stream, LB_SETTOPINDEX, cnt - 1, 0);
  }
}

// 把高亮拨回详情区正在展示的那条推理（程序化设置不触发点击逻辑）
static void stream_select_current() {
  if (g_view < 0 || g_view >= (int)g_invs.size()) return;
  int seq = g_invs[g_view].seq;
  for (int i = (int)g_stream.size() - 1; i >= 0; i--) {
    if (g_stream[i].inv && g_stream[i].seq == seq) {
      g_suppress_sel = true;
      SendMessageW(h_stream, LB_SETCURSEL, i, 0);
      g_suppress_sel = false;
      return;
    }
  }
}

// 推理行的前 3 词补齐（C 行到达时；按 seq 找流行）
static void stream_update_inv(const Inv &inv) {
  if (inv.rows.size() > 3) return;
  std::wstring txt = inv_row_text(inv);
  for (int i = (int)g_stream.size() - 1; i >= 0; i--) {
    if (g_stream[i].inv && g_stream[i].seq == inv.seq) {
      g_stream[i].text = txt;
      if (!g_paused) {
        // 原地改行的唯一可靠做法：删了重插（LB_SETITEMDATA 不能改文本）。
        // DELETESTRING 会把选中清掉（cursel → -1）→ 重插后把选中补回该行。
        int cnt = (int)SendMessageW(h_stream, LB_GETCOUNT, 0, 0);
        if (i < cnt) {
          int cur = (int)SendMessageW(h_stream, LB_GETCURSEL, 0, 0);
          SendMessageW(h_stream, LB_DELETESTRING, i, 0);
          SendMessageW(h_stream, LB_INSERTSTRING, i, (LPARAM)txt.c_str());
          g_suppress_sel = true;
          SendMessageW(h_stream, LB_SETCURSEL, (WPARAM)cur, 0);  // 重插后行号不变
          g_suppress_sel = false;
        }
      }
      break;
    }
  }
}

// 整列重建（暂停恢复 / 清空后；g_stream 是唯一事实源）
static void stream_rebuild() {
  SendMessageW(h_stream, LB_RESETCONTENT, 0, 0);
  for (auto &e : g_stream)
    SendMessageW(h_stream, LB_ADDSTRING, 0, (LPARAM)e.text.c_str());
  stream_select_current();   // 高亮回到详情区当前那条推理
}

// ---- 协议行处理（UI 线程）----
static void handle_line(const std::string &line) {
  if (line == "::CONNECTED") {
    g_connected.store(true);
    if (!g_paused) set_status(L"● 已连接", true);
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
    // 协议握手行仅作识别；版本/日期徽章自 2026-11-16 不再显示
    //（状态行保持「● 已连接」），行本身保留供协议升级判别
    return;
  }
  if (f[0] == "L" && f.size() >= 10) {
    Inv inv;
    inv.seq = ++g_inv_seq;
    inv.t = u8w(f[1]);
    inv.input = u8w(f[2]);
    inv.cache = u8w(f[3]);
    inv.src = u8w(f[4]);
    inv.ctx = u8w(f[5]);
    inv.tick = u8w(f[6]);
    inv.ms = u8w(f[7]);
    inv.ready = u8w(f[8]);
    inv.prep = f.size() > 10 ? u8w(f[10]) : L"";
    // 协议 v4（2026-11-12）：推理用尾部文本（灰显）。2026-11-17 起服务端
    // 删掉 n_seen 字段、ctx_used 前移一位——按字段数兼容：13 字段（旧 DLL）
    // 取第 13 字段，12 字段（新行）取第 12 字段；都没有 = 空（不灰显）。
    inv.ctxused = f.size() > 12 ? u8w(f[12])
                  : f.size() > 11 ? u8w(f[11]) : L"";
    // 协议 L 行第 10 字段 n（候选数，信息行 2026-11-16 起不再显示）保留忽略
    g_invs.push_back(std::move(inv));
    while (g_invs.size() > 50) {
      g_invs.pop_front();
      if (g_view > 0) g_view--;
    }
    g_view = (int)g_invs.size() - 1;
    if (!g_paused) {
      Inv &back = g_invs.back();
      stream_add(true, inv_row_text(back), back.seq);
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
    r.eff = u8w(f[5]);
    r.uni = u8w(f[6]);
    r.fused = u8w(f[7]);
    r.full = u8w(f[8]);
    r.cerank = u8w(f[9]);
    r.fin = u8w(f[10]);
    r.win = u8w(f[11]);
    r.win0 = (r.win == L"0");
    if (f.size() > 14) {  // 协议 v2：CE 三分量原始值（未乘权重）
      r.ce1 = u8w(f[12]);
      r.ce2 = u8w(f[13]);
      r.ce3 = u8w(f[14]);
    }
    if (f.size() > 16) {  // 协议 v3：词频得分 / 累计得分（真正进融合的两项）
      r.fb = u8w(f[15]);
      r.u1 = u8w(f[16]);
    }
    if (f.size() > 17)  // 协议 v5：旧修正分 λ（负分幅度, 0=未命中）
      r.pf = u8w(f[17]);
    if (f.size() > 18)  // 协议 v6：有符号修正分（+200/+100/-5/0）
      r.sdelta = u8w(f[18]);
    g_invs.back().rows.push_back(std::move(r));
    if (!g_paused) {
      if (g_view == (int)g_invs.size() - 1) update_current();
      stream_update_inv(g_invs.back());   // 合并流推理行补前 3 词
    }
    return;
  }
  wchar_t tmp[512];
  if (f[0] == "S" && f.size() >= 3) {
    swprintf(tmp, 512, L"%s 跳过 %s（输入 %s 来源 %s）", u8w(f[1]).c_str(),
             u8w(f[2]).c_str(), f.size() > 3 ? u8w(f[3]).c_str() : L"",
             f.size() > 4 ? u8w(f[4]).c_str() : L"-");
    stream_add(false, tmp, 0);
    return;
  }
  if (f[0] == "E" && f.size() >= 7) {
    swprintf(tmp, 512, L"%s 上屏 %s  eff %s→%s  tick %s→%s", u8w(f[1]).c_str(),
             u8w(f[2]).c_str(), u8w(f[3]).c_str(), u8w(f[4]).c_str(),
             u8w(f[5]).c_str(), u8w(f[6]).c_str());
    stream_add(false, tmp, 0);
    return;
  }
  if (f[0] == "R" && f.size() >= 3) {
    swprintf(tmp, 512, L"%s 重置 %s", u8w(f[1]).c_str(), u8w(f[2]).c_str());
    stream_add(false, tmp, 0);
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

// 上文行控件 = 只读 RichEdit（2026-11-12 灰显，协议 v4）：STATIC 无分段
// 着色能力；Msftedit 动态加载（系统组件），缺失退回 STATIC（不灰显仅全文）。
// 竖滚动条承接长上文（原 STATIC 单行省略号退役）
static HWND mk_ctx(int x, int y, int w, int h, int id, HFONT f) {
  static bool msft_tried = false;
  if (!msft_tried) {
    msft_tried = true;
    LoadLibraryW(L"Msftedit.dll");
  }
  HWND c = CreateWindowW(L"RichEdit50W", L"上文：—",
                         WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                             WS_VSCROLL,
                         ts(x), ts(y), ts(w), ts(h), g_hwnd,
                         (HMENU)(INT_PTR)id, NULL, NULL);
  if (c) {
    g_ctx_is_rich = true;
    SendMessageW(c, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
  } else {
    c = mk_ctl(L"STATIC", L"上文：—", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, x, y,
               w, h, id, f);
  }
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
  // 表格等宽字体：分数格内"得分=计算过程"的各成分才能逐位对齐
  //（中文经 GDI 字体链接回退，数字/符号本身是 Consolas）；高度与插件版
  // 同为 12px（9pt）——列宽按这个步进取值，13px 会截断"衰减词频分"格
  g_font_mono = CreateFontW(-ts(12), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Consolas");

  h_status = mk_ctl(L"STATIC", L"等待连接…（在方案开启 llm_rerank/debug_fusion"
                                L" 并重新部署，然后打字）",
                    SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 8, 700, 22,
                    IDC_STATUS, g_font);
  h_info = mk_ctl(L"STATIC", L"—", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 34,
                  1074, 20, IDC_INFO, g_font_bold);
  h_ctx = mk_ctx(8, 58, 1074, 42, IDC_CTX, g_font);
  mk_ctl(L"BUTTON", L"暂停", BS_PUSHBUTTON, 720, 6, 64, 24, IDC_PAUSE, g_font);
  mk_ctl(L"BUTTON", L"置顶", BS_AUTOCHECKBOX, 792, 8, 52, 22, IDC_TOP, g_font);
  mk_ctl(L"BUTTON", L"清空", BS_PUSHBUTTON, 878, 6, 52, 24, IDC_CLEAR, g_font);

  h_grid = mk_ctl(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                      LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                  8, 104, 1078, 250, IDC_GRID, g_font_mono);
  ListView_SetExtendedListViewStyle(
      h_grid, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
  // 分数部分五列（2026-11-11 用户定案"分数=计算过程"）：CE / 衰减词频分 /
  // 累计词频 / 总分 / 修正分，格内 = 得分 + '=' + 具体计算（成分统一 3 位小数，
  // 等宽字体逐位对齐），底部公式行随之删除；身份列 = #/词。
  // 「窗」「匹配词长」列均已删（2026-11-16 用户定案：窗内/尾窗灰字区分、
  // 满长准入优先下"有无 CE 分"即满长标志——列冗余）；
  // 修正分为专属列（取代 v5 总分格尾追 −λ，pf=0 可见）。
  // 列宽按 GDI 实测取（TextRenderer：数字 ~7px/字符，'·' 字体链接后
  // 14px 全角——比 WinForms 宽一倍），含 3 位整数的最坏情况余量。
  // 插入循环上界按数组长度推导——曾把循环写死 i<10 而 11 列，第 11 列
  // 从未创建（表现为"最后一列消失"，2026-11-11 踩过）
  struct { const wchar_t *t; int w; } cols[] = {
      {L"#", 26}, {L"词", 64}, {L"总分", 64}, {L"CE", 310},
      {L"衰减词频分", 140}, {L"累计词频分", 112},
      {L"修正分", 64}};  // 总分 64 = 纯分值（v5 的 90 容 −λ 标注已不需要）
  LVCOLUMNW col = {};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  for (int i = 0; i < (int)(sizeof(cols) / sizeof(cols[0])); i++) {
    col.pszText = (LPWSTR)cols[i].t;
    col.cx = ts(cols[i].w);
    col.iSubItem = i;
    ListView_InsertColumn(h_grid, i, &col);
  }

  // 合并流（2026-11-17 用户定案）：原「历史」（推理项列表，右侧 204 宽）与
  // 「事件」（纯日志）两块合一，网格下方通栏。owner-draw 只为分色：推理行
  // ◆ 黑字、日志行灰字；选中高亮 = 详情区所示那条推理。
  h_stream = mk_ctl(L"LISTBOX", L"",
                    WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL |
                        LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED |
                        LBS_HASSTRINGS,
                    8, 360, 1078, 188, IDC_STREAM, g_font);
  SendMessageW(h_stream, LB_SETITEMHEIGHT, 0, (LPARAM)ts(18));
  // 打字测试框（2026-11-16 用户定案）：在此输入即走本机 IME——组词/候选/
  // 上屏全程在本窗口内完成，逐候选数据实时刷新，免切窗口测试。
  // 标准多行 EDIT，TSF 对其自动生效（同记事本）；合并流下方。
  mk_ctl(L"STATIC", L"打字测试（在此输入即走本机 IME，逐候选数据实时刷新）",
         SS_LEFT, 8, 552, 460, 16, IDC_TYPELBL, g_font_hint);
  mk_ctl(L"EDIT", L"",
         WS_BORDER | WS_VSCROLL | WS_TABSTOP | ES_LEFT | ES_MULTILINE |
             ES_AUTOVSCROLL | ES_WANTRETURN,
         8, 572, 1078, 64, IDC_TYPE, g_font);
  // 公式行/列义行已删（2026-11-11 用户定案：计算过程直接写在分数格里，
  // 无需再对照底部公式）；只剩数据源一行灰注
  h_foot = mk_ctl(L"STATIC",
                  L"数据源 \\\\.\\pipe\\rime_llm_live · 开关 = 方案 "
                  L"llm_rerank/debug_fusion（改后重新部署）· 预解 = 上文 KV 预解码"
                  L"命中",
                  SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 644, 1078, 16, IDC_FOOT,
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
      RECT cr = {0, 0, ts(1094), ts(664)};
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
            // 恢复：整列重建合并流（暂停期间 g_stream 照常入队、ListBox 已陈旧）
            stream_rebuild();
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
          g_stream.clear();
          g_view = -1;
          SendMessageW(h_stream, LB_RESETCONTENT, 0, 0);
          update_current();
          return 0;
        case IDCANCEL:
          DestroyWindow(h);
          return 0;
      }
      // 合并流点击（2026-11-17）：推理行（◆）→ 详情切到该条；日志行不可点开——
      // 高亮弹回详情区当前那条（行为同合并前的历史列表，只是混进了日志行）
      if (LOWORD(wp) == IDC_STREAM && HIWORD(wp) == LBN_SELCHANGE) {
        if (g_suppress_sel) return 0;
        int sel = (int)SendMessageW(h_stream, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= (int)g_stream.size()) return 0;
        if (!g_stream[sel].inv) {   // 纯日志行：弹回
          stream_select_current();
          return 0;
        }
        for (int j = (int)g_invs.size() - 1; j >= 0; j--) {
          if (g_invs[j].seq == g_stream[sel].seq) {
            g_view = j;
            update_current();
            return 0;
          }
        }
        stream_select_current();   // 该推理已滚出 50 条窗口 → 保持当前视图
        return 0;
      }
      break;
    case WM_NOTIFY: {
      NMHDR *nm = (NMHDR *)lp;
      if (nm->idFrom == IDC_GRID && nm->code == NM_CUSTOMDRAW)
        return on_grid_customdraw((NMLVCUSTOMDRAW *)lp);
      // CE 子项自 2026-11-11 起**常显**（原点击展开已删：NM_CLICK 的
      // iSubItem 在 report 视图里恒为 0，需 LVM_SUBITEMHITTEST 才准）。
      break;
    }
    case WM_CTLCOLORSTATIC:
      // 白底：STATIC 默认用 COLOR_BTNFACE 擦底（灰条）——透明 + 返回白刷
      SetBkMode((HDC)wp, TRANSPARENT);
      return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_MEASUREITEM: {
      // 合并流行高跟 DPI（owner-draw fixed）
      MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
      if (mi && mi->CtlID == IDC_STREAM) {
        mi->itemHeight = (UINT)ts(18);
        return TRUE;
      }
      break;
    }
    case WM_DRAWITEM: {
      // 合并流 owner-draw：只为分色——推理行 ◆ 黑字、日志行灰字
      //（同尾窗灰约定）；选中 = 系统高亮
      DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
      if (d && d->CtlID == IDC_STREAM && d->itemID != (UINT)-1) {
        std::wstring txt;
        if (d->itemID < g_stream.size()) txt = g_stream[d->itemID].text;
        bool sel = (d->itemState & ODS_SELECTED) != 0;
        bool inv = (d->itemID < g_stream.size()) && g_stream[d->itemID].inv;
        HBRUSH bg = sel ? GetSysColorBrush(COLOR_HIGHLIGHT)
                        : (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(d->hDC, &d->rcItem, bg);
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                 : (inv ? RGB(0, 0, 0) : RGB(150, 150, 150)));
        RECT r = d->rcItem;
        r.left += ts(2);
        DrawTextW(d->hDC, txt.c_str(), (int)txt.size(), &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (d->itemState & ODS_FOCUS) DrawFocusRect(d->hDC, &d->rcItem);
        return TRUE;
      }
      break;
    }
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
                           CW_USEDEFAULT, CW_USEDEFAULT, ts(1094), ts(548),
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
