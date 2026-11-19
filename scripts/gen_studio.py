#!/usr/bin/env python3
"""gen_studio.py — 生成 WeaselLLMStudio.cpp（设置+调试 二合一窗口，单界面并排）。

用户定案（2026-11-16）："写在一个界面里，不要切换"——**无 Tab、无页切换**：
  左列 = 设置页（原 WeaselLLMSetup 布局逐行保留，620 设计宽，~592 高）
  右列 = 调试页（原 WeaselLLMDebug 数据栈逐行保留；坐标重排为纵向堆叠：
         状态/信息/上文 → 网格 → 历史 → 事件 → 打字测试 → 页脚）
客户区 1548×706 设计像素（用户 3120@200% 屏 = 1560 逻辑宽，恰好放下）。

从 weasel/WeaselLLMSetup/WeaselLLMSetup.cpp 与 weasel/WeaselLLMDebug/
WeaselLLMDebug.cpp 机械拼装生成，两个源文件保持不动；可重复运行。

变换清单（全部 count 断言，失配即报错不写出）：
  设置页：出错日志改名 Studio / make_ui 拆 studio_base_fonts + cfg_make_ui /
          mk・mk_group・mk_hint・mk_help 注入 g_cfg_x/g_cfg_y 页原点偏移
  调试页：IDC_STATUS→IDC_DSTATUS / TIMER_DRAIN→DBG_TIMER_ID(5) /
          set_status→dbg_status / g_font*→d_font*（正则词界） /
          删本文件 g_hwnd 重复声明与 ts() 定义 / make_ui→dbg_make_ui /
          12 处坐标重排（右列原点 644，宽 896，历史/事件移到网格下方）
  壳层：  合并消息处理（Enter 默认按钮 = 原设置页口径『保存并生效』；
          打字测试框 ES_WANTRETURN 框内回车换行不触发）
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SETUP = ROOT / "weasel" / "WeaselLLMSetup" / "WeaselLLMSetup.cpp"
DEBUG = ROOT / "weasel" / "WeaselLLMDebug" / "WeaselLLMDebug.cpp"
OUT = ROOT / "weasel" / "WeaselLLMStudio" / "WeaselLLMStudio.cpp"

# Windows 控制台默认 GBK；生成器的进度行含 ✓ 等非 GBK 字符，
# 不处理会在**文件已写出后**抛 UnicodeEncodeError（exit 1 → bat 误判 GEN FAILED）
sys.stdout.reconfigure(errors="replace")


def must_sub(text, old, new, n):
    got = text.count(old)
    if got != n:
        sys.exit(f"FATAL: expect {n} occurrence(s), got {got} for: {old[:90]!r}")
    return text.replace(old, new)


def must_re(text, pat, repl, at_least=1):
    new, cnt = re.subn(pat, repl, text)
    if cnt < at_least:
        sys.exit(f"FATAL: regex matched {cnt} < {at_least}: {pat}")
    return new, cnt


def slice_between(lines, start_marker, end_marker):
    a = next(i for i, l in enumerate(lines) if l.startswith(start_marker))
    b = next(i for i, l in enumerate(lines) if l.startswith(end_marker))
    return lines[a:b]


HEADER = r'''// WeaselLLMStudio.cpp — LLM 设置+调试 二合一窗口（2026-11-16 用户指令：
// "将配置窗口和调试窗口二合一，以便提高调试效率……原来的 2 个窗口代码保留，
//  新窗口另外写一个文件"；同日定案"写在一个界面里，不要切换"）。
//
// **单界面并排**（无 Tab 无切换）：本文件由 scripts\gen_studio.py 从两个现有
// 源码机械拼装生成——勿手改拼装区，改两原文件后重跑生成器。
//   左列【设置】= WeaselLLMSetup.cpp 原控件/逻辑逐行保留（620 设计宽，
//              页原点偏移 g_cfg_x/g_cfg_y 由 mk/mk_group/mk_hint/mk_help
//              统一注入，页内相对几何零变化）；
//   右列【调试】= WeaselLLMDebug.cpp 原协议栈/数据模型逐行保留，坐标重排
//              为纵向堆叠（网格 7 列宽度不动）：状态行+暂停/置顶/清空 →
//              信息行 → 上文行 → 候选网格(250 高) → 合并流(188：推理项 ◆
//              可点开详情 + 灰字日志行，两块合一 2026-11-17) → 打字测试框
//              → 页脚。符号改名避开设置页同名：
//              IDC_STATUS→IDC_DSTATUS、set_status→dbg_status、g_font*→
//              d_font*、TIMER_DRAIN→DBG_TIMER_ID=5（原 1 与下载定时器
//              DL_TIMER_ID 冲突）；
//   壳层     = 两窗口消息处理并集。Enter 默认按钮沿用原设置页口径 =
//              『保存并生效』（打字测试框 ES_WANTRETURN，框内回车换行不触发）。
// 布局：客户区 1548×706 设计像素（用户 3120@200% 屏 = 1560 逻辑宽，恰好
// 放下；100% 屏 1548px < 1920）。左列 8..628，右列 644..1540。
// 构建：scripts\build_llm_studio.bat（cl 直编，模式同 build_llm_setup.bat）。
// 核对钩子：WEASEL_LLM_STUDIO_DPI（强制按指定 DPI 排版；依次回退
// WEASEL_LLM_SETUP_DPI、WEASEL_LLM_DEBUG_DPI、系统 DPI）。
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <objbase.h>
#include <gdiplus.h>   // 抗锯齿圆（GDI 的 Ellipse/RoundRect 无 AA，小圆锯齿明显）
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "comctl32.lib")  // 悬停提示（tooltips_class32）+ ListView
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")   // 「?」圆徽标 / 按钮圆角（系统组件）
#pragma comment(lib, "advapi32.lib")  // RegGetValue（WeaselRoot 定位部署器）
// 视觉样式（Common Controls v6）：否则按钮/勾选框呈 Win2000 经典浮雕样式
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- 二合一：设置页页原点（设备像素；由壳层 WM_CREATE 赋值）----
static int g_cfg_x = 0;   // 左列 x 起点（ts(8)）
static int g_cfg_y = 0;   // 左列 y 起点（ts(8)，顶对齐）
'''

SHELL = r'''
// ===================【壳：合并消息处理（单界面：左设置 + 右调试）】==================
// 实际消息处理（外面再套一层 WndProc 捕获异常 → 写错误日志）
static LRESULT CALLBACK WndProcInner(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_hwnd = h;
      studio_base_fonts();            // g_font 三件套 + tooltip（原 make_ui 头部）
      g_cfg_x = ts(8);
      g_cfg_y = ts(8);
      cfg_make_ui();                  // 左列设置页
      dbg_make_ui();                  // 右列调试页
      scan_schemas();        // 填方案下拉（默认选第一个）
      load_schema_params();  // 读该方案的 llm_rerank 节 → 界面
      // 客户区 1548×706（右列调试内容底 = 页脚 672+16；极端字体下取设置页
      // 内容高与 706 的大者，避免左列底部被裁）
      int ch = ts(706);
      if (g_ui_height + ts(16) > ch) ch = g_ui_height + ts(16);
      RECT cr = {0, 0, ts(1548), ch};
      AdjustWindowRect(&cr, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE);
      SetWindowPos(h, NULL, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      std::thread(reader_thread).detach();   // 管道读取（原 WeaselLLMDebug）
      SetTimer(h, DBG_TIMER_ID, 100, NULL);
      // 忘关提醒（2026-09-04）：诊断开着会持续写盘，打开设置时点一下
      if (g_p.debug_fusion)
        set_status(GetDlgItem(h, IDC_STATUS),
                   L"提醒：诊断日志开着（rime_llm_debug.txt 持续增长，排障完建议关闭）");
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDC_MODEL &&
          (HIWORD(wp) == CBN_EDITUPDATE || HIWORD(wp) == CBN_SELCHANGE)) {
        refresh_model_status();  // 手输/下拉选择/浏览返回即刷新
        return 0;
      }
      // 切换方案 = 切换配置（配置节在方案里，2026-09-30 定案）
      if (LOWORD(wp) == IDC_SCHEMA && HIWORD(wp) == CBN_SELCHANGE) {
        if (g_sel_guard) return 0;
        // ④ 有未保存改动先问；选"否"把下拉拨回已加载的那一项，界面值原样保留
        if (ui_dirty() && MessageBoxW(g_hwnd,
                L"当前方案有未保存的改动。\n\n切换方案会丢弃这些改动，继续吗？",
                L"未保存的改动", MB_YESNO | MB_ICONWARNING) != IDYES) {
          // 按**名字**拨回而不是按序号：列表可能刚被重扫过（序号会错位）
          g_sel_guard = true;
          if (!g_loaded_name.empty()) select_schema_named(g_loaded_name);
          g_sel_guard = false;
          set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"已取消切换（保留未保存的改动）");
          return 0;
        }
        load_schema_params();
        return 0;
      }
      if (LOWORD(wp) == IDC_SCHEMA && HIWORD(wp) == CBN_DROPDOWN) {
        // 打开下拉即重扫两处方案列表（外部新增/删除方案无需手动刷新）；
        // 界面没有未保存改动时顺带重读当前文件（吸收外部编辑），有改动则只重扫列表
        scan_schemas();
        if (!ui_dirty()) load_schema_params();
        return 0;
      }
      switch (LOWORD(wp)) {
        // —— 设置列（原 WeaselLLMSetup）——
        case IDC_BROWSE: on_browse(); return 0;
        case IDC_BENCH: on_bench_threads(); return 0;
        case IDC_DOWNLOAD: on_download_model(); return 0;
        case IDC_SCHEMAREM: on_schema_remove(); return 0;
        case IDC_SAVE: save_schema_params(); return 0;
        case IDC_CLOSE: DestroyWindow(h); return 0;
        // —— 调试列（原 WeaselLLMDebug）——
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
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
          else
            SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
          return 0;
        case IDC_CLEAR:
          g_invs.clear();
          g_stream.clear();
          g_view = -1;
          SendMessageW(h_stream, LB_RESETCONTENT, 0, 0);
          update_current();
          return 0;
        case IDCANCEL:            // IsDialogMessage 把 ESC 映射为 IDCANCEL
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
    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)lp;
      // 合并流 owner-draw（2026-11-17）：只为分色——推理行 ◆ 黑字、日志行
      // 灰字；选中 = 系统高亮（同 WeaselLLMDebug 独立窗口同款绘制）
      if (d && d->CtlType == ODT_LISTBOX && d->CtlID == IDC_STREAM &&
          d->itemID != (UINT)-1) {
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
      if (d && d->CtlType == ODT_STATIC) {  // 只有「?」徽标是 owner-draw static
        draw_help(d);
        return TRUE;
      }
      if (d && d->CtlType == ODT_BUTTON) {  // 全部按钮都是自绘（mk_button）
        draw_button(d);
        return TRUE;
      }
      break;
    }
    // 静态文本/复选框默认用 COLOR_BTNFACE（灰）擦自己的底，而窗口类是
    // COLOR_WINDOW（白）→ 白底上一条条灰底（2026-10-01 用户报「label 背景
    // 是灰的」）。统一返回窗口背景刷 + 透明文字底 → 标签与整体底色一致。
    case WM_CTLCOLORSTATIC: {
      HDC dc = (HDC)wp;
      SetBkMode(dc, TRANSPARENT);
      return (LRESULT)(HBRUSH)(COLOR_WINDOW + 1);
    }
    // 自绘按钮与 BS_DEFPUSHBUTTON 抢同一个样式位（BS_TYPEMASK 低 4 位）→ 缺省
    // 按钮标记丢失，回车就没有落点。这里手动回答"IsDialogMessage 的默认按钮是
    // 谁"，回车仍然 = 『保存并生效』（原设置页 2026-10-01 口径；打字测试框是
    // ES_WANTRETURN 多行框，框内回车换行本就不走默认按钮）。
    case DM_GETDEFID:
      return MAKELRESULT(IDC_SAVE, DC_HASDEFID);
    case DM_SETDEFID:
      return 0;
    case WM_TIMER:
      if (wp == DL_TIMER_ID) { on_download_tick(); return 0; }
      if (wp == BENCH_TIMER_ID) { on_bench_tick(); return 0; }
      if (wp == DBG_TIMER_ID) {
        // 管道行排水（原 WeaselLLMDebug WM_TIMER）
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
        return 0;
      }
      break;
    case WM_DESTROY:
      KillTimer(h, DL_TIMER_ID);
      KillTimer(h, BENCH_TIMER_ID);
      KillTimer(h, DBG_TIMER_ID);
      // ② 关窗连带结束 curl：它每次都是新进程，留着会和下次下载同时写同一个分片。
      //    分片本身保留 → 下次点『下载模型』照样断点续传。
      if (g_dl_proc) { TerminateProcess(g_dl_proc, 0); CloseHandle(g_dl_proc); g_dl_proc = NULL; }
      if (!g_dl_tmp.empty()) kill_stale_curl(g_dl_tmp);
      // 线程数实测同理：子进程不带孙子进程，直接结束 + 删 pid（结果文件留着无妨）
      if (g_bench_proc) { TerminateProcess(g_bench_proc, 0); CloseHandle(g_bench_proc); g_bench_proc = NULL; }
      if (!g_bench_pid.empty()) DeleteFileW(g_bench_pid.c_str());
      // 管道读取线程收尾（原 WeaselLLMDebug）
      g_stop.store(true);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// 顶层异常兜底（2026-09-30：界面不留日志框，异常写 WeaselLLMStudio_error.log）
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  try {
    return WndProcInner(h, msg, wp, lp);
  } catch (const std::exception& e) {
    log_error(L"GUI 未捕获异常（msg=0x%04X）：%S", (unsigned)msg, e.what());
  } catch (...) {
    log_error(L"GUI 未捕获异常（msg=0x%04X）：未知类型", (unsigned)msg);
  }
  set_status(GetDlgItem(h, IDC_STATUS),
             L"[失败] 界面异常，已写错误日志：WeaselLLMStudio_error.log");
  return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  // 主线程 STA：Vista+ 打开/保存对话框内部走 COM，缺初始化会静默失败
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  // GDI+：圆徽标与按钮圆角的抗锯齿（失败则退回 GDI 画法，见 draw_help/draw_button）
  Gdiplus::GdiplusStartupInput gdipInput;
  GdiplusStartup(&g_gdip, &gdipInput, NULL);
  // 悬停提示 + ListView（两原窗口的初始化集并集）
  INITCOMMONCONTROLSEX icc = {sizeof(icc),
                              ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES |
                                  ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  WNDCLASSW wc = {0};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"WeaselLLMStudio";
  RegisterClassW(&wc);
  // 初始尺寸随意（WM_CREATE 里按 1548×706 校正）
  HWND h = CreateWindowExW(WS_EX_APPWINDOW, L"WeaselLLMStudio",
                           L"LLM 设置与调试 — 小狼毫",
                           WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                           CW_USEDEFAULT, CW_USEDEFAULT, ts(1548), ts(740),
                           NULL, NULL, inst, NULL);
  ShowWindow(h, show);
  UpdateWindow(h);
  MSG m;
  while (GetMessageW(&m, NULL, 0, 0) > 0) {
    if (!IsDialogMessageW(h, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  if (g_gdip) Gdiplus::GdiplusShutdown(g_gdip);
  CoUninitialize();
  return 0;
}
'''


def transform_setup(txt: str) -> str:
    # 出错日志文件名 → Studio（error_log_path 两处 + make_ui 页脚注）
    txt = must_sub(txt, "WeaselLLMSetup_error.log", "WeaselLLMStudio_error.log", 3)
    # make_ui 拆分：头部（字体三件套 + tip_init）→ studio_base_fonts，
    # 主体（const int PAD 起）→ cfg_make_ui
    txt = must_sub(txt, "static void make_ui() {",
                   "static void studio_base_fonts() {", 1)
    pad_line = ("  const int PAD = ts(12);   // 分组框外边距（左右对称：客户区 620 - 2×12 = 596）")
    txt = must_sub(
        txt, pad_line,
        "}\n\n"
        "// 左列设置页布局（原 make_ui 主体拆出：字体/tooltip 在 studio_base_fonts()；\n"
        "// 页原点偏移 g_cfg_x/g_cfg_y 由 mk/mk_group/mk_hint/mk_help 统一注入）\n"
        "static void cfg_make_ui() {\n" + pad_line, 1)
    # mk()：页原点偏移
    txt = must_sub(txt, "WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hwnd,",
                   "WS_CHILD | WS_VISIBLE | style, x + g_cfg_x, y + g_cfg_y, w, h, g_hwnd,", 1)
    # mk_group()
    txt = must_sub(txt, "WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, w, h,",
                   "WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x + g_cfg_x, y + g_cfg_y, w, h,", 1)
    # mk_hint()
    txt = must_sub(txt, "CreateWindowW(L\"STATIC\", text, WS_CHILD | WS_VISIBLE, x, y, w, ts(18),",
                   "CreateWindowW(L\"STATIC\", text, WS_CHILD | WS_VISIBLE, x + g_cfg_x, y + g_cfg_y, w, ts(18),", 1)
    # mk_help()
    txt = must_sub(txt, "WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, x, y,",
                   "WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, x + g_cfg_x, y + g_cfg_y,", 1)
    # ts()：DPI 核对钩子三级链 STUDIO→SETUP→DEBUG（头注释承诺的口径）
    txt = must_sub(
        txt,
        '    wchar_t env[16] = L"";\n'
        '    DWORD n = GetEnvironmentVariableW(L"WEASEL_LLM_SETUP_DPI", env, 16);\n'
        '    if (n > 0 && n < 16) dpi = _wtoi(env);\n',
        '    wchar_t env[16] = L"";\n'
        '    DWORD n = GetEnvironmentVariableW(L"WEASEL_LLM_STUDIO_DPI", env, 16);\n'
        '    if (n <= 0 || n >= 16)\n'
        '      n = GetEnvironmentVariableW(L"WEASEL_LLM_SETUP_DPI", env, 16);\n'
        '    if (n <= 0 || n >= 16)\n'
        '      n = GetEnvironmentVariableW(L"WEASEL_LLM_DEBUG_DPI", env, 16);\n'
        '    if (n > 0 && n < 16) dpi = _wtoi(env);\n', 1)
    return txt


def transform_debug(txt: str) -> str:
    # ID 改名
    txt = must_sub(txt, "#define IDC_STATUS 2001", "#define IDC_DSTATUS 2001", 1)
    n = txt.count("IDC_STATUS")
    txt = txt.replace("IDC_STATUS", "IDC_DSTATUS")
    print(f"  debug: IDC_STATUS→IDC_DSTATUS ×{n + 1}")
    txt = must_sub(txt, "#define TIMER_DRAIN 1", "#define DBG_TIMER_ID 5", 1)
    # set_status → dbg_status（定义 + CONNECTED/DISCONNECTED 两处调用）
    n = txt.count("set_status(")
    if n != 3:
        sys.exit(f"FATAL: dbg set_status expect 3, got {n}")
    txt = txt.replace("set_status(", "dbg_status(")
    # 字体全局 g_font* → d_font*（词界正则；长名先替换防前缀吞并）
    for pat, rep in [(r"\bg_font_mono\b", "d_font_mono"),
                     (r"\bg_font_bold\b", "d_font_bold"),
                     (r"\bg_font_hint\b", "d_font_hint"),
                     (r"\bg_font\b", "d_font")]:
        txt, cnt = must_re(txt, pat, rep, 1)
        print(f"  debug: {pat} → {rep} ×{cnt}")
    # 删本文件 g_hwnd 重复声明（壳层共用设置页的声明）
    txt = must_sub(txt, "static HWND g_hwnd = NULL;\n", "", 1)
    # make_ui → dbg_make_ui（mk_ctx 定义之后紧跟，锚点唯一）
    txt = must_sub(txt, "}\n\nstatic void make_ui() {",
                   "}\n\nstatic void dbg_make_ui() {", 1)
    # —— 12 处坐标重排：右列原点 644，内容宽 896（652..1540），历史/事件
    #    移到网格下方纵向堆叠；网格 7 列宽度不动（GDI 实测值，见原注释）——
    coords = [
        # 状态行（右侧让位给三按钮：1356 起排）
        ("SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 8, 700, 22,",
         "SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 652, 8, 688, 22,", 1),
        # 信息行
        ("SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 34,\n                  1074, 20, IDC_INFO,",
         "SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 652, 34,\n                  888, 20, IDC_INFO,", 1),
        # 上文行
        ("h_ctx = mk_ctx(8, 58, 1074, 42, IDC_CTX,",
         "h_ctx = mk_ctx(652, 58, 888, 42, IDC_CTX,", 1),
        # 暂停/置顶/清空（右缘 1540 对齐：1488+52 / 1428+52 / 1356+64）
        ("BS_PUSHBUTTON, 720, 6, 64, 24, IDC_PAUSE,",
         "BS_PUSHBUTTON, 1356, 6, 64, 24, IDC_PAUSE,", 1),
        ("BS_AUTOCHECKBOX, 792, 8, 52, 22, IDC_TOP,",
         "BS_AUTOCHECKBOX, 1428, 8, 52, 22, IDC_TOP,", 1),
        ("BS_PUSHBUTTON, 878, 6, 52, 24, IDC_CLEAR,",
         "BS_PUSHBUTTON, 1488, 6, 52, 24, IDC_CLEAR,", 1),
        # 网格（7 列合计 780 < 888；高 250，2026-11-17 合并流腾出高度）
        ("8, 104, 1078, 250, IDC_GRID,", "652, 104, 888, 250, IDC_GRID,", 1),
        # 合并流（2026-11-17：原历史列 + 事件栏合一，网格下方通栏）
        ("8, 360, 1078, 188, IDC_STREAM,", "652, 360, 888, 188, IDC_STREAM,", 1),
        # 打字测试标签 + 输入框
        ("SS_LEFT, 8, 552, 460, 16, IDC_TYPELBL,",
         "SS_LEFT, 652, 552, 460, 16, IDC_TYPELBL,", 1),
        ("8, 572, 1078, 64, IDC_TYPE,", "652, 572, 888, 64, IDC_TYPE,", 1),
        # 页脚（数据源灰注）
        ("SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 8, 644, 1078, 16, IDC_FOOT,",
         "SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 652, 644, 888, 16, IDC_FOOT,", 1),
    ]
    for old, new, n in coords:
        txt = must_sub(txt, old, new, n)
    print(f"  debug: {len(coords)} 处坐标重排完成")
    return txt


def main():
    setup_lines = SETUP.read_text(encoding="utf-8").splitlines()
    dbg_lines = DEBUG.read_text(encoding="utf-8").splitlines()

    # —— 设置页：从 "// ---- 控件 ID ----" 到 WndProcInner 注释前 ——
    setup_seg = slice_between(setup_lines, "// ---- 控件 ID ----",
                              "// 实际消息处理（外面再套一层 WndProc 捕获异常")
    setup_txt = transform_setup("\n".join(setup_seg) + "\n")

    # —— 调试页：ID 块 + kPipeName + 小工具→UI 构建 + customdraw（跳过 ts 定义
    #    与 WndProc/wWinMain——壳层统一 ts（三环境变量链）与消息处理）——
    dbg_ids = slice_between(dbg_lines, "// ---- 控件 ID ----", "#define TIMER_DRAIN 1")
    dbg_ids.append("#define TIMER_DRAIN 1")
    pipe = next(l for l in dbg_lines if l.startswith("static const wchar_t *kPipeName"))
    small = slice_between(dbg_lines, "// ---- 小工具 ----",
                          "// 尾窗透传行灰字（NM_CUSTOMDRAW）")
    tail = slice_between(dbg_lines, "// 尾窗透传行灰字（NM_CUSTOMDRAW）",
                         "static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {")
    dbg_txt = transform_debug("\n".join(dbg_ids) + "\n" + pipe + "\n\n" +
                              "\n".join(small) + "\n" + "\n".join(tail) + "\n")

    # —— 调试页 ID 块的节注释改名（防与设置页注释混淆）——
    dbg_txt = must_sub(dbg_txt, "// ---- 控件 ID ----",
                       "// ---- 调试列控件 ID（原 WeaselLLMDebug；IDC_STATUS→IDC_DSTATUS、\n"
                       "//      TIMER_DRAIN→DBG_TIMER_ID=5 避让设置页同名/同值定时器）----", 1)

    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(HEADER + setup_txt + "\n" +
                   "// ===================【右列调试：原 WeaselLLMDebug.cpp 全量并入】===================\n" +
                   dbg_txt + SHELL, encoding="utf-8", newline="\n")
    n = OUT.read_text(encoding="utf-8").count("\n")
    print(f"OK: {OUT}  ({n} lines)")

    # —— 生成后自检 ——
    full = OUT.read_text(encoding="utf-8")
    for marker in ("static int ts(int v)", "int WINAPI wWinMain",
                   "static void dbg_make_ui()", "static void cfg_make_ui()",
                   "static void studio_base_fonts()"):
        cnt = full.count(marker)
        if cnt != 1:
            sys.exit(f"FATAL: self-check {marker!r} = {cnt}, want 1")
    for banned in ("WC_TABCONTROLW", "switch_tab", "IDC_TAB", "page_reg",
                   "g_pages", "WEASEL_LLM_STUDIO_TAB"):
        if banned in full:
            sys.exit(f"FATAL: tab-era residual {banned!r} present")
    code_residual = [l for l in full.splitlines()
                     if "TIMER_DRAIN" in l and not l.lstrip().startswith("//")]
    if code_residual:
        sys.exit(f"FATAL: TIMER_DRAIN code residual: {code_residual}")
    print("  check TIMER_DRAIN: only in comments ✓")
    print("self-check passed")


if __name__ == "__main__":
    main()
