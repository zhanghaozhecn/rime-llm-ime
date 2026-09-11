// probe_uia_ctx.cpp — UIA TextPattern 光标上文探针（独立实验，不依赖/不修改小狼毫）
// 验证命题（2026-09-10 插件版上文可行性调研）：外部进程经 UIA 读"焦点控件
// 光标前文"是否可行、覆盖哪些应用、延迟量级——与 COM 旁路同构的候选上文源
// （UIA 通用现代应用 + COM 补 Office 类 + 历史兜底 = 插件版三层上文）。
//   默认   单次探测当前焦点元素：进程/类名/Name + TextPattern/ValuePattern 能力 +
//          光标前 64 字（DocumentRange.Start → Selection.Start → 前移 64 字）+ 耗时
//   -loop  每 2s 循环——在各应用里打字，观察前文是否跟随（同 probe_wps_ctx）
// 编译：build_probe_uia_ctx.bat（cl /utf-8，链接 UIAutomationCore）

#include <windows.h>
#include <objbase.h>
#include <UIAutomationClient.h>  // SDK 拆分头: 客户端接口(IUIAutomationTextPattern
#include <UIAutomationCore.h>    // 等)在 Client.h; TextUnit 枚举在 Core.h
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "UIAutomationCore.lib")

static void uprintf(const wchar_t* fmt, ...) {
  wchar_t wbuf[2048];
  va_list ap; va_start(ap, fmt);
  _vsnwprintf_s(wbuf, _TRUNCATE, fmt, ap);
  va_end(ap);
  char ubuf[4096];
  int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, ubuf, sizeof(ubuf), NULL, NULL);
  if (n > 0) fwrite(ubuf, 1, n - 1, stdout);
  fflush(stdout);
}

static double now_ms() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
  return c.QuadPart * 1000.0 / f.QuadPart;
}

static void prog_name_of(DWORD pid, wchar_t* out, int cch) {
  out[0] = 0;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return;
  wchar_t p[MAX_PATH];
  DWORD nc = MAX_PATH;
  if (QueryFullProcessImageNameW(h, 0, p, &nc)) {
    wchar_t* base = wcsrchr(p, L'\\');
    wcscpy_s(out, cch, base ? base + 1 : p);
  }
  CloseHandle(h);
}

// 光标前文: DocumentRange.Start → Selection.Start，起点再前移 64 字符
static bool text_before_caret(IUIAutomation* uia, IUIAutomationElement* el,
                              wchar_t* out, int cch, int* caret_at) {
  IUIAutomationTextPattern* tp = NULL;
  if (FAILED(el->GetCurrentPattern(UIA_TextPatternId, (IUnknown**)&tp)) || !tp)
    return false;
  IUIAutomationTextRangeArray* sel = NULL;
  if (FAILED(tp->GetSelection(&sel)) || !sel)
    { if (tp) tp->Release(); return false; }
  IUIAutomationTextRange* s0 = NULL;
  int nsel = 0; sel->get_Length(&nsel); if (nsel > 0)
    sel->GetElement(0, &s0);
  sel->Release();
  if (!s0) { tp->Release(); return false; }

  IUIAutomationTextRange* r = NULL;
  bool ret = false;
  do {
    IUIAutomationTextRange* doc = NULL;
    if (FAILED(tp->get_DocumentRange(&doc)) || !doc) break;
    if (FAILED(doc->Clone(&r))) { doc->Release(); break; }
    // r = [doc.Start, sel.Start] = 光标(或选区起点)之前的全部文本
    r->MoveEndpointByRange(TextPatternRangeEndpoint_Start,
                           doc, TextPatternRangeEndpoint_Start);
    r->MoveEndpointByRange(TextPatternRangeEndpoint_End,
                           s0, TextPatternRangeEndpoint_Start);
    doc->Release();
    int moved = 0;
    r->MoveEndpointByUnit(TextPatternRangeEndpoint_Start,
                          TextUnit_Character, -64, &moved);
    (void)moved;
    BSTR txt = NULL;
    if (FAILED(r->GetText(-1, &txt)) || !txt) break;
    int len = SysStringLen(txt);
    if (len >= cch) len = cch - 1;
    wcsncpy_s(out, cch, txt, len);
    out[len] = 0;
    if (caret_at) *caret_at = len;
    SysFreeString(txt);
    ret = true;
  } while (false);
  if (r) r->Release();
  s0->Release();
  tp->Release();
  return ret;
}

static IUIAutomation* g_uia = NULL;

static void probe_once() {
  double t0 = now_ms();
  IUIAutomationElement* el = NULL;
  if (FAILED(g_uia->GetFocusedElement(&el)) || !el) {
    uprintf(L"[FAIL] GetFocusedElement 失败\n");
    return;
  }
  wchar_t name[256] = L"", cls[128] = L"";
  BSTR bn = NULL;
  if (SUCCEEDED(el->get_CurrentName(&bn)) && bn) {
    wcsncpy_s(name, 256, bn, _TRUNCATE);
    SysFreeString(bn);
  }
  BSTR bc = NULL;
  if (SUCCEEDED(el->get_CurrentClassName(&bc)) && bc) {
    wcsncpy_s(cls, 128, bc, _TRUNCATE);
    SysFreeString(bc);
  }
  int pid = 0;
  el->get_CurrentProcessId(&pid);
  wchar_t prog[128];
  prog_name_of((DWORD)pid, prog, 128);
  // IsTextPatternAvailable 无 Current 快捷 getter —— 走通用属性查询
  VARIANT vt, vv;
  VariantInit(&vt); VariantInit(&vv);
  BOOL has_text = FALSE, has_value = FALSE;
  if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_IsTextPatternAvailablePropertyId, &vt)) &&
      vt.vt == VT_BOOL)
    has_text = vt.boolVal;
  if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_IsValuePatternAvailablePropertyId, &vv)) &&
      vv.vt == VT_BOOL)
    has_value = vv.boolVal;
  VariantClear(&vt); VariantClear(&vv);
  double t_probe = now_ms() - t0;

  wchar_t before[256] = L"";
  int caret = 0;
  bool got = has_text && text_before_caret(g_uia, el, before, 256, &caret);
  double t_all = now_ms() - t0;

  uprintf(L"[%s] %5.1fms  proc=%-18s cls=%-26s name=%.24s  value=%s  before=[%s]\n",
          got ? L"OK" : (has_text ? L"TP" : (has_value ? L"VAL" : L"--")),
          t_all, prog, cls, name,
          has_value ? L"y" : L"n",
          before);
  el->Release();
}

int main(int argc, char** argv) {
  bool loop = false;
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "-loop")) loop = true;
  SetConsoleOutputCP(CP_UTF8);
  if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                              IID_IUIAutomation, (void**)&g_uia))) {
    uprintf(L"CUIAutomation 创建失败\n");
    return 1;
  }
  probe_once();
  if (loop) {
    uprintf(L"—— loop 模式：切到各应用打字，观察前文跟随（Ctrl+C 退出）——\n");
    for (;;) {
      Sleep(2000);
      SYSTEMTIME st; GetLocalTime(&st);
      uprintf(L"%02u:%02u:%02u ", st.wHour, st.wMinute, st.wSecond);
      probe_once();
    }
  }
  g_uia->Release();
  CoUninitialize();
  return 0;
}
