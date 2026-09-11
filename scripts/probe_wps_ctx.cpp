// probe_wps_ctx.cpp — WPS 光标上文获取探针（独立实验，不依赖/不修改小狼毫）
// 对应 memory/wps-context-investigation.md 第十一节决定性实验清单：
//   默认    实验 2/3/4：ProgID 解析 → GetActiveObject(ROT 附着，前台/失焦态均可跑)
//            → 失败则 CoCreateInstance(单实例附着/类厂测试) → 读
//            ActiveDocument / Selection.Start / Range(start-64,start).Text，全链计时
//   -loop    每 2s 循环读前文——WPS 里打字，观察输出是否实时跟随（附着态持续可用性）
//   -msaa    实验 5：枚举 WPS 主窗口全部子孙 HWND，逐个 AccessibleObjectFromWindow
//            （= 对画布直发 WM_GETOBJECT）→ accRole/accName/accValue +
//            QI IAccessible2 / IAccessibleText——闭环"孤岛对象"盲区
// 编译：build_probe_wps_ctx.bat（cl /utf-8，链接 ole32 oleaut32 oleacc user32）

#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <oleacc.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "user32.lib")

// IA2 公开标准 GUID（accessibility-api.org 规范；若负结果意外存疑，先核对 GUID 再下结论）
static const IID kIID_IAccessible2 = {0xE89F726E, 0xC4F4, 0x4c19, {0xBB, 0x19, 0xB6, 0x47, 0xD7, 0xFA, 0x84, 0x78}};
static const IID kIID_IAccessibleText = {0x241267D5, 0x5353, 0x4c60, {0x8E, 0xFD, 0x89, 0x06, 0x33, 0x61, 0x33, 0x93}};

// ---- UTF-8 控制台输出（源码 /utf-8，宽串统一转 UTF-8 打印）----
static void uprintf(const wchar_t* fmt, ...) {
  wchar_t wbuf[2048];
  va_list ap; va_start(ap, fmt);
  _vsnwprintf_s(wbuf, _TRUNCATE, fmt, ap);
  va_end(ap);
  char ubuf[4096];
  int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, ubuf, sizeof(ubuf), NULL, NULL);
  if (n > 0) fwrite(ubuf, 1, n - 1, stdout);
  fflush(stdout);   // stdout 重定向到文件时是全缓冲——loop 模式必须逐行落盘
}

static double now_ms() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
  return c.QuadPart * 1000.0 / f.QuadPart;
}

// ---- IDispatch 精简封装 ----
static HRESULT disp_invoke(IDispatch* obj, const wchar_t* name, WORD flags,
                           DISPPARAMS* pdp, VARIANT* ret) {
  DISPID dispid = 0;
  HRESULT hr = obj->GetIDsOfNames(IID_NULL, (LPOLESTR*)&name, 1, LOCALE_USER_DEFAULT, &dispid);
  if (FAILED(hr)) { uprintf(L"    [x] GetIDsOfNames(%s) hr=0x%08lX\n", name, (unsigned long)hr); return hr; }
  VariantInit(ret);
  EXCEPINFO ei; ZeroMemory(&ei, sizeof(ei));
  UINT argerr = 0;
  hr = obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, pdp, ret, &ei, &argerr);
  if (FAILED(hr)) {
    uprintf(L"    [x] Invoke(%s) hr=0x%08lX", name, (unsigned long)hr);
    if (hr == DISP_E_EXCEPTION) {
      if (ei.bstrDescription) uprintf(L"  server: %s", ei.bstrDescription);
      else if (ei.scode) uprintf(L"  scode=0x%08lX", (unsigned long)ei.scode);
    }
    uprintf(L"\n");
    SysFreeString(ei.bstrSource); SysFreeString(ei.bstrDescription); SysFreeString(ei.bstrHelpFile);
  }
  return hr;
}

static HRESULT disp_get(IDispatch* obj, const wchar_t* name, VARIANT* ret) {
  DISPPARAMS dp; ZeroMemory(&dp, sizeof(dp));
  return disp_invoke(obj, name, DISPATCH_PROPERTYGET, &dp, ret);
}

// 双 I4 参数方法（doc.Range(start, end)）；rgvark 反序：末参数在前
static HRESULT disp_range(IDispatch* doc, long start, long end, VARIANT* ret) {
  VARIANT args[2];
  VariantInit(&args[0]); args[0].vt = VT_I4; args[0].lVal = end;
  VariantInit(&args[1]); args[1].vt = VT_I4; args[1].lVal = start;
  DISPPARAMS dp; ZeroMemory(&dp, sizeof(dp));
  dp.rgvarg = args; dp.cArgs = 2;
  return disp_invoke(doc, L"Range", DISPATCH_METHOD | DISPATCH_PROPERTYGET, &dp, ret);
}

// 无参方法/取属性调用（Documents.Add）
static HRESULT disp_call0(IDispatch* obj, const wchar_t* name, VARIANT* ret) {
  DISPPARAMS dp; ZeroMemory(&dp, sizeof(dp));
  return disp_invoke(obj, name, DISPATCH_METHOD, &dp, ret);
}

// 单 BSTR 参数方法（Selection.TypeText）
static HRESULT disp_call_bstr(IDispatch* obj, const wchar_t* name, const wchar_t* s, VARIANT* ret) {
  VARIANT a; VariantInit(&a); a.vt = VT_BSTR; a.bstrVal = SysAllocString(s);
  DISPPARAMS dp; ZeroMemory(&dp, sizeof(dp));
  dp.rgvarg = &a; dp.cArgs = 1;
  HRESULT hr = disp_invoke(obj, name, DISPATCH_METHOD, &dp, ret);
  VariantClear(&a);
  return hr;
}

// 单 I4 参数方法（Document.Close(0) = wdDoNotSaveChanges）
static HRESULT disp_call_i4(IDispatch* obj, const wchar_t* name, long v, VARIANT* ret) {
  VARIANT a; VariantInit(&a); a.vt = VT_I4; a.lVal = v;
  DISPPARAMS dp; ZeroMemory(&dp, sizeof(dp));
  dp.rgvarg = &a; dp.cArgs = 1;
  return disp_invoke(obj, name, DISPATCH_METHOD, &dp, ret);
}

// ---- 实验主链：附着 → 读光标前 64 字 ----
static IDispatch* g_app = NULL;   // loop 模式缓存 Application

static void probe_once(bool verbose) {
  // 读取链：app.ActiveWindow → win.Selection → sel.Start →
  //          app.ActiveDocument → doc → doc.Range(start-64, start).Text
  double t0 = now_ms();
  VARIANT vwin, vsel, vstart, vdoc, vname, vrng, vtxt;
  VariantInit(&vwin); VariantInit(&vsel); VariantInit(&vstart);
  VariantInit(&vdoc); VariantInit(&vname); VariantInit(&vrng); VariantInit(&vtxt);
  bool ok = false;
  do {
    if (FAILED(disp_get(g_app, L"ActiveWindow", &vwin)) || vwin.vt != VT_DISPATCH || !vwin.pdispVal) break;
    if (FAILED(disp_get(vwin.pdispVal, L"Selection", &vsel)) || vsel.vt != VT_DISPATCH || !vsel.pdispVal) break;
    if (FAILED(disp_get(vsel.pdispVal, L"Start", &vstart)) || vstart.vt != VT_I4) break;
    if (FAILED(disp_get(g_app, L"ActiveDocument", &vdoc)) || vdoc.vt != VT_DISPATCH || !vdoc.pdispVal) break;
    long pos = vstart.lVal, from = pos - 64; if (from < 0) from = 0;
    if (FAILED(disp_range(vdoc.pdispVal, from, pos, &vrng)) || vrng.vt != VT_DISPATCH || !vrng.pdispVal) break;
    if (FAILED(disp_get(vrng.pdispVal, L"Text", &vtxt)) || vtxt.vt != VT_BSTR) break;
    if (FAILED(disp_get(vdoc.pdispVal, L"Name", &vname))) VariantInit(&vname);
    uprintf(L"[OK] %6.2fms  doc=%s  pos=%ld  before=[%s]\n",
            now_ms() - t0,
            (vname.vt == VT_BSTR ? vname.bstrVal : L"?"),
            pos, vtxt.bstrVal);
    ok = true;
  } while (false);
  if (!ok) uprintf(L"[FAIL] 读取链失败（%.2fms）——通常=无打开文档或对象模型受限\n", now_ms() - t0);
  VariantClear(&vwin); VariantClear(&vsel); VariantClear(&vstart);
  VariantClear(&vdoc); VariantClear(&vname); VariantClear(&vrng); VariantClear(&vtxt);
}

static int attach_app(bool allow_create) {
  CLSID clsid;
  HRESULT hr = CLSIDFromProgID(L"Kwps.Application", &clsid);
  if (FAILED(hr)) { uprintf(L"[实验1] ProgID 解析失败 hr=0x%08lX\n", (unsigned long)hr); return 1; }
  wchar_t clsid_str[64]; StringFromGUID2(clsid, clsid_str, 64);
  uprintf(L"[实验1] Kwps.Application -> CLSID %ls\n", clsid_str);

  IUnknown* punk = NULL;
  hr = GetActiveObject(clsid, NULL, &punk);
  uprintf(L"[实验2] GetActiveObject(ROT 附着) hr=0x%08lX %s\n",
          (unsigned long)hr, SUCCEEDED(hr) ? L"成功" : L"失败");
  if (SUCCEEDED(hr)) {
    hr = punk->QueryInterface(IID_IDispatch, (void**)&g_app);
    punk->Release();
    if (SUCCEEDED(hr)) return 0;
    uprintf(L"  [x] QI(IDispatch) hr=0x%08lX\n", (unsigned long)hr);
  }
  if (!allow_create) return 1;
  hr = CoCreateInstance(clsid, NULL, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                        IID_IDispatch, (void**)&g_app);
  uprintf(L"[实验3] CoCreateInstance(单实例附着/类厂) hr=0x%08lX %s\n",
          (unsigned long)hr, SUCCEEDED(hr) ? L"成功" : L"失败");
  if (FAILED(hr)) return 1;
  // 判据：Documents.Count 是否等于已开文档数（=附着现有实例；0 且弹新进程 = 新起）
  VARIANT vdocs, vcount;
  if (SUCCEEDED(disp_get(g_app, L"Documents", &vdocs)) && vdocs.vt == VT_DISPATCH) {
    if (SUCCEEDED(disp_get(vdocs.pdispVal, L"Count", &vcount)) && vcount.vt == VT_I4)
      uprintf(L"  Documents.Count = %ld（对照 WPS 已开文档数判断附着/新起）\n", vcount.lVal);
    VariantClear(&vcount); VariantClear(&vdocs);
  }
  return 0;
}

// ---- selftest：全自动闭环（Add → TypeText → 读前文 → Close → Quit）----
static const wchar_t* kSelfText = L"LLM重排光标上文获取实验：The quick brown fox jumps over the lazy dog 0123456789 尾部标记XYZ";

static int run_selftest() {
  CLSID clsid;
  if (FAILED(CLSIDFromProgID(L"Kwps.Application", &clsid))) { uprintf(L"ProgID 解析失败\n"); return 1; }
  IUnknown* punk = NULL;
  HRESULT hr = GetActiveObject(clsid, NULL, &punk);
  uprintf(L"[selftest] 前: GetActiveObject hr=0x%08lX\n", (unsigned long)hr);
  if (SUCCEEDED(hr)) { punk->QueryInterface(IID_IDispatch, (void**)&g_app); punk->Release(); }
  else if (FAILED(CoCreateInstance(clsid, NULL, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                                   IID_IDispatch, (void**)&g_app))) {
    uprintf(L"[selftest] 附着失败\n"); return 1;
  }
  VARIANT vdocs, vdoc, vwin, vsel;
  VariantInit(&vdocs); VariantInit(&vdoc); VariantInit(&vwin); VariantInit(&vsel);
  bool doc_open = false;
  do {
    if (FAILED(disp_get(g_app, L"Documents", &vdocs)) || vdocs.vt != VT_DISPATCH) break;
    uprintf(L"[selftest] Documents.Add …\n");
    if (FAILED(disp_call0(vdocs.pdispVal, L"Add", &vdoc)) || vdoc.vt != VT_DISPATCH) break;
    doc_open = true;
    if (FAILED(disp_get(g_app, L"ActiveWindow", &vwin)) || vwin.vt != VT_DISPATCH) break;
    if (FAILED(disp_get(vwin.pdispVal, L"Selection", &vsel)) || vsel.vt != VT_DISPATCH) break;
    uprintf(L"[selftest] Selection.TypeText …\n");
    VARIANT vt; VariantInit(&vt);
    if (FAILED(disp_call_bstr(vsel.pdispVal, L"TypeText", kSelfText, &vt))) { VariantClear(&vt); break; }
    VariantClear(&vt);
  } while (false);
  if (!doc_open) { uprintf(L"[selftest] 打开测试文档失败\n"); return 1; }

  probe_once(false);   // 读链（光标在 TypeText 之后，前文应含输入尾部）

  // 文档开着的此刻再探一次 ROT（KB238610 语义：焦点态/有文档态注册？）
  IUnknown* pk2 = NULL;
  hr = GetActiveObject(clsid, NULL, &pk2);
  uprintf(L"[selftest] 后(文档已开): GetActiveObject hr=0x%08lX %s\n",
          (unsigned long)hr, SUCCEEDED(hr) ? L"成功" : L"失败");
  if (pk2) pk2->Release();

  // 清理：不保存关闭 + 退出自动化实例
  VARIANT vt2; VariantInit(&vt2);
  disp_call_i4(vdoc.pdispVal, L"Close", 0, &vt2); VariantClear(&vt2);
  VARIANT vt3; VariantInit(&vt3);
  disp_call0(g_app, L"Quit", &vt3); VariantClear(&vt3);
  uprintf(L"[selftest] 已清理（Close 不保存 + Quit）\n");
  VariantClear(&vdocs); VariantClear(&vdoc); VariantClear(&vwin); VariantClear(&vsel);
  return 0;
}
static int g_msaa_seen = 0;

static void try_msaa(HWND h) {
  IAccessible* pacc = NULL;
  HRESULT hr = AccessibleObjectFromWindow(h, OBJID_CLIENT, IID_IAccessible, (void**)&pacc);
  if (FAILED(hr) || !pacc) return;   // WM_GETOBJECT 无应答 = 无 MSAA 对象（多数窗口）
  g_msaa_seen++;
  wchar_t cls[128] = L""; GetClassNameW(h, cls, 128);
  VARIANT child; VariantInit(&child); child.vt = VT_I4; child.lVal = CHILDID_SELF;
  VARIANT vrole;
  VariantInit(&vrole);
  BSTR bname = NULL, bvalue = NULL;
  pacc->get_accRole(child, &vrole);
  pacc->get_accName(child, &bname);
  pacc->get_accValue(child, &bvalue);
  IUnknown* p2 = NULL; IUnknown* pt = NULL;
  HRESULT hr2 = pacc->QueryInterface(kIID_IAccessible2, (void**)&p2);
  HRESULT hrt = pacc->QueryInterface(kIID_IAccessibleText, (void**)&pt);
  uprintf(L"  hwnd=%p cls=%-24s role=%ld name=[%s] value=[%.40s]  IA2=%s IAccessibleText=%s\n",
          h, cls,
          (vrole.vt == VT_I4 ? vrole.lVal : -1),
          (bname ? bname : L""),
          (bvalue ? bvalue : L""),
          SUCCEEDED(hr2) ? L"YES" : L"no",
          SUCCEEDED(hrt) ? L"YES" : L"no");
  if (p2) p2->Release();
  if (pt) pt->Release();
  SysFreeString(bname); SysFreeString(bvalue);
  VariantClear(&vrole);
  pacc->Release();
}

static BOOL CALLBACK enum_children(HWND h, LPARAM) { try_msaa(h); return TRUE; }

struct FindCtx { const wchar_t* cls; HWND found; };
static BOOL CALLBACK enum_top(HWND h, LPARAM lp) {
  FindCtx* ctx = (FindCtx*)lp;
  wchar_t cls[128];
  if (IsWindowVisible(h) && GetClassNameW(h, cls, 128) && wcscmp(cls, ctx->cls) == 0) {
    ctx->found = h; return FALSE;
  }
  return TRUE;
}

static void run_msaa() {
  FindCtx ctx = { L"OpusApp", NULL };
  EnumWindows(enum_top, (LPARAM)&ctx);
  if (!ctx.found) { uprintf(L"[实验5] 未找到 OpusApp 主窗口（WPS 文字未打开？）\n"); return; }
  uprintf(L"[实验5] OpusApp 主窗口 %p，枚举全部子孙窗口（AccessibleObjectFromWindow = WM_GETOBJECT 直发）\n", ctx.found);
  try_msaa(ctx.found);
  EnumChildWindows(ctx.found, enum_children, 0);
  uprintf(L"[实验5] 有 MSAA 对象的窗口共 %d 个。判定：任一 value 含正文或 IAccessibleText=YES → 盲区实存；否则方法 3 关死\n", g_msaa_seen);
}

// ---- ROT 全枚举：GetActiveObject 只按 CLSID 查——WPS 可能注册的是文件
// moniker（Word 语义：每文档一条），全枚举才能看到用户实例的注册 ----
static void rot_enumerate() {
  IRunningObjectTable* prot = NULL;
  if (FAILED(GetRunningObjectTable(0, &prot))) { uprintf(L"[rot] GetRunningObjectTable 失败\n"); return; }
  IEnumMoniker* pem = NULL;
  if (FAILED(prot->EnumRunning(&pem))) { prot->Release(); uprintf(L"[rot] EnumRunning 失败\n"); return; }
  IBindCtx* pbc = NULL; CreateBindCtx(0, &pbc);
  IMoniker* mk[1]; ULONG fetched = 0; int total = 0, wpsish = 0;
  while (pem->Next(1, mk, &fetched) == S_OK && fetched == 1) {
    LPOLESTR dn = NULL;
    if (SUCCEEDED(mk[0]->GetDisplayName(pbc, NULL, &dn)) && dn) {
      total++;
      // 只详列 WPS 相关（kso/wps/kingsoft/docx/wps后缀）；其余计数
      wchar_t lower[1024];
      int i; for (i = 0; dn[i] && i < 1023; i++) lower[i] = towlower(dn[i]); lower[i] = 0;
      if (wcsstr(lower, L"wps") || wcsstr(lower, L"kso") || wcsstr(lower, L"kingsoft") ||
          wcsstr(lower, L".docx") || wcsstr(lower, L".doc") || wcsstr(lower, L".et") ||
          wcsstr(lower, L".dps") || wcsstr(lower, L".txt") || wcsstr(lower, L"000209ff")) {
        wpsish++;
        IUnknown* pobj = NULL;
        if (SUCCEEDED(prot->GetObject(mk[0], &pobj))) {
          IDispatch* pd = NULL;
          if (SUCCEEDED(pobj->QueryInterface(IID_IDispatch, (void**)&pd))) {
            uprintf(L"[rot] %ls   (IDispatch OK)\n", dn);
            pd->Release();
          } else {
            uprintf(L"[rot] %ls   (无 IDispatch)\n", dn);
          }
          pobj->Release();
        } else uprintf(L"[rot] %ls   (GetObject 失败)\n", dn);
      }
    }
    if (dn) CoTaskMemFree(dn);
    mk[0]->Release();
  }
  uprintf(L"[rot] 共 %d 条，WPS/文档相关 %d 条\n", total, wpsish);
  pbc->Release(); pem->Release(); prot->Release();
}

// ---- main ----
int main(int argc, char** argv) {
  bool loop = false, msaa = false, selftest = false, rot = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-loop")) loop = true;
    else if (!strcmp(argv[i], "-msaa")) msaa = true;
    else if (!strcmp(argv[i], "-selftest")) selftest = true;
    else if (!strcmp(argv[i], "-rot")) rot = true;
  }
  SetConsoleOutputCP(CP_UTF8);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (msaa) {
    run_msaa();
  } else if (rot) {
    rot_enumerate();
  } else if (selftest) {
    return run_selftest();
  } else if (attach_app(true) != 0) {
    uprintf(L"附着失败，实验终止（先跑 -msaa 或 -selftest）\n");
    CoUninitialize();
    return 1;
  } else {
    probe_once(true);
    if (loop) {
      uprintf(L"—— loop 模式：在 WPS 文档里打字，观察前文是否跟随（Ctrl+C 退出）——\n");
      for (;;) {
        Sleep(2000);
        SYSTEMTIME st; GetLocalTime(&st);
        uprintf(L"%02u:%02u:%02u ", st.wHour, st.wMinute, st.wSecond);
        if (!g_app) { if (attach_app(false) != 0) { uprintf(L"  (附着失败，重试中)\n"); continue; } }
        probe_once(false);
      }
    }
  }
  if (g_app) g_app->Release();
  CoUninitialize();
  return 0;
}
