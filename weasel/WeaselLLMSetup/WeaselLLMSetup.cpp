// WeaselLLMSetup.cpp — LLM 重排设置（托盘菜单“LLM 重排设置”启动）
//
// 直接安装版（2026-08-27）：读写 %APPDATA%\Rime\llm_rerank.yaml（平面
// key: value）。llm_filter（librime）对该文件热重载——保存即生效，无需
// 重新部署。参数三级优先级：schema llm_rerank 节 > 本文件 > 内置默认。
// 模型下载：curl.exe -L -C - 断点续传（.download 分片，完成转正）——
// 与安装器 download-model 同源设计。全程用户态，无需管理员。
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
// 视觉样式（Common Controls v6）：否则按钮/勾选框呈 Win2000 经典浮雕样式
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- 控件 ID ----
#define IDC_ENABLED      1001
#define IDC_MODEL        1002
#define IDC_BROWSE       1003
#define IDC_DOWNLOAD     1004
#define IDC_DLSTATUS     1005
// 数值参数（列1/列2 各自顺序）
#define IDC_MIN_CODE     1011
#define IDC_MAX_CODE     1012
#define IDC_MIN_TOK      1013
#define IDC_MAX_TOK      1014
#define IDC_CORES        1015
#define IDC_ELW          1021
#define IDC_FREQ_W       1022
#define IDC_FREQ_K       1023
#define IDC_MAX_CAND     1024
#define IDC_SAVE         1101
#define IDC_CLOSE        1102
#define IDC_STATUS       1103

static const wchar_t* MODEL_URL =
    L"https://modelscope.cn/models/unsloth/Qwen3.5-0.8B-GGUF/resolve/master/"
    L"Qwen3.5-0.8B-Q4_K_M.gguf";

struct Params {
  bool enabled = false;
  int min_code_len = 4, max_code_len = 0;
  int min_tokens = 1, max_tokens = 10, cpu_cores = 4;
  double elw = 0.0, freq_weight = 0.25;
  int freq_k = 5, max_candidates = 5;
  std::wstring model_path;  // 显示值（空 = 默认路径）
};
static Params g_p;
static HWND g_hwnd;
static HFONT g_font;
static HANDLE g_curl = NULL;       // 下载进程
static std::wstring g_dl_tmp;      // .download 分片路径

// ---- 工具 ----
static std::wstring yaml_path() {
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH))
    return std::wstring();
  return std::wstring(dir) + L"\\Rime";
}

static std::wstring default_model_path() {
  // 默认 = 用户文件夹（2026-08-27 用户定案：不假设存在 D: 分区；
  // 有 D 盘模型的机器在 GUI/schema 里显式填 D:\gguf_models\...）
  wchar_t up[MAX_PATH];
  if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH))
    return std::wstring(up) + L"\\gguf_models\\Qwen3.5-0.8B-Q4_K_M.gguf";
  return L"C:\\gguf_models\\Qwen3.5-0.8B-Q4_K_M.gguf";
}

static std::wstring shown_model() {
  return g_p.model_path.empty() ? default_model_path() : g_p.model_path;
}

static std::string wide_to_utf8(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 0)
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
  return s;
}
static std::wstring utf8_to_wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}
static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// ---- llm_rerank.yaml 读写（与 llm_filter.cc 的扁平解析同构）----
static void load_params() {
  std::wstring dir = yaml_path();
  if (dir.empty()) return;
  FILE* f = NULL;
  _wfopen_s(&f, (dir + L"\\llm_rerank.yaml").c_str(), L"rb");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    std::string ln = trim(line);
    if (ln.empty() || ln[0] == '#') continue;
    size_t c = ln.find(':');
    if (c == std::string::npos) continue;
    std::string key = trim(ln.substr(0, c));
    std::string val = trim(ln.substr(c + 1));
    if (!val.empty() && val[0] == '"') {
      size_t e = val.find('"', 1);
      val = (e == std::string::npos) ? val.substr(1) : val.substr(1, e - 1);
    } else {
      size_t h = val.find('#');
      if (h != std::string::npos) val = trim(val.substr(0, h));
    }
    if (key == "enabled") g_p.enabled = (val == "true");
    else if (key == "min_code_len") g_p.min_code_len = atoi(val.c_str());
    else if (key == "max_code_len") g_p.max_code_len = atoi(val.c_str());
    else if (key == "expected_length_weight") g_p.elw = atof(val.c_str());
    else if (key == "freq_weight") g_p.freq_weight = atof(val.c_str());
    else if (key == "freq_k") g_p.freq_k = atoi(val.c_str());
    else if (key == "min_tokens") g_p.min_tokens = atoi(val.c_str());
    else if (key == "max_tokens") g_p.max_tokens = atoi(val.c_str());
    else if (key == "max_candidates") g_p.max_candidates = atoi(val.c_str());
    else if (key == "cpu_cores") g_p.cpu_cores = atoi(val.c_str());
    else if (key == "model_path") g_p.model_path = utf8_to_wide(val);
  }
  fclose(f);
}

static bool save_params() {
  std::wstring dir = yaml_path();
  if (dir.empty()) return false;
  CreateDirectoryW(dir.c_str(), NULL);
  std::wstring model = shown_model();
  char buf[64];
  std::string out;
  out += "# LLM 重排全局配置（WeaselLLMSetup 写入；llm_filter 热重载即时生效）\n";
  out += "# 优先级：方案内 llm_rerank 节 > 本文件 > 内置默认\n";
  out += g_p.enabled ? "enabled: true\n" : "enabled: false\n";
  sprintf_s(buf, "min_code_len: %d\n", g_p.min_code_len); out += buf;
  sprintf_s(buf, "max_code_len: %d # 0=不限制\n", g_p.max_code_len); out += buf;
  sprintf_s(buf, "expected_length_weight: %.2f\n", g_p.elw); out += buf;
  sprintf_s(buf, "freq_weight: %.2f\n", g_p.freq_weight); out += buf;
  sprintf_s(buf, "freq_k: %d\n", g_p.freq_k); out += buf;
  sprintf_s(buf, "min_tokens: %d\n", g_p.min_tokens); out += buf;
  sprintf_s(buf, "max_tokens: %d\n", g_p.max_tokens); out += buf;
  sprintf_s(buf, "max_candidates: %d\n", g_p.max_candidates); out += buf;
  sprintf_s(buf, "cpu_cores: %d\n", g_p.cpu_cores); out += buf;
  out += "model_path: " + wide_to_utf8(model) + "\n";
  std::wstring tmp = dir + L"\\llm_rerank.yaml.tmp";
  FILE* f = NULL;
  _wfopen_s(&f, tmp.c_str(), L"wb");
  if (!f) return false;
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
  // 原子替换：热重载按 mtime|size 指纹感知；tmp 同目录保证同卷 rename
  if (!MoveFileExW(tmp.c_str(), (dir + L"\\llm_rerank.yaml").c_str(),
                   MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

// ---- 下载（curl.exe 断点续传，分片转正）----
static bool file_size(const std::wstring& p, unsigned long long* sz) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa))
    return false;
  *sz = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
  return true;
}

static void set_status(HWND ctrl, const wchar_t* fmt, ...) {
  wchar_t buf[512];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  SetWindowTextW(ctrl, buf);
}

// 逐级建目录（接受 / 或 \ 分隔；SDK 的 SHCreateDirectoryExW 在部分宏组合下不声明）
static bool ensure_parent_dir(const std::wstring& model) {
  std::wstring dir = model;
  size_t p = dir.find_last_of(L"/\\");
  if (p == std::wstring::npos) return true;
  dir.resize(p);
  if (dir.size() < 3) return true;  // 盘根 C:\ 无需创建
  // 规范为反斜杠逐级尝试
  for (auto& c : dir)
    if (c == L'/') c = L'\\';
  for (size_t i = 3; i <= dir.size(); ++i) {
    if (i != dir.size() && dir[i] != L'\\') continue;
    std::wstring part = dir.substr(0, i);
    DWORD attr = GetFileAttributesW(part.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
      if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
      continue;  // 已存在
    }
    if (!CreateDirectoryW(part.c_str(), NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
      return false;
  }
  return true;
}

static void start_download() {
  if (g_curl) return;
  std::wstring model = shown_model();
  g_dl_tmp = model + L".download";
  if (!ensure_parent_dir(model)) {
    set_status(GetDlgItem(g_hwnd, IDC_DLSTATUS),
               L"无法创建模型目录，请检查模型路径是否可用");
    return;
  }
  wchar_t cmd[2048];
  _snwprintf_s(cmd, _TRUNCATE,
               L"curl.exe -L -C - -s -S -o \"%s\" \"%s\"", g_dl_tmp.c_str(),
               MODEL_URL);
  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    set_status(GetDlgItem(g_hwnd, IDC_DLSTATUS), L"无法启动 curl.exe（Win10 1803+ 自带）");
    return;
  }
  CloseHandle(pi.hThread);
  g_curl = pi.hProcess;
  SetTimer(g_hwnd, 1, 500, NULL);
  set_status(GetDlgItem(g_hwnd, IDC_DLSTATUS), L"开始下载（约 500MB，断点续传，可关闭窗口不影响）…");
}

static void poll_download() {
  if (!g_curl) { KillTimer(g_hwnd, 1); return; }
  unsigned long long sz = 0;
  file_size(g_dl_tmp, &sz);
  set_status(GetDlgItem(g_hwnd, IDC_DLSTATUS), L"已下载 %llu MB / 约 508 MB", sz >> 20);
  if (WaitForSingleObject(g_curl, 0) != WAIT_OBJECT_0) return;
  // 进程退出
  DWORD code = 1;
  GetExitCodeProcess(g_curl, &code);
  CloseHandle(g_curl);
  g_curl = NULL;
  KillTimer(g_hwnd, 1);
  if (code == 0 && sz > 100 * 1024 * 1024) {
    if (MoveFileExW(g_dl_tmp.c_str(), shown_model().c_str(),
                    MOVEFILE_REPLACE_EXISTING)) {
      set_status(GetDlgItem(g_hwnd, IDC_DLSTATUS), L"下载完成：%s", shown_model().c_str());
      return;
    }
  }
  set_status(GetDlgItem(g_hwnd, IDC_DLSTATUS),
             L"下载失败（curl 退出码 %lu）；分片已保留，重试可续传", code);
}

// ---- 控件读写 ----
static void ui_to_params() {
  g_p.enabled = SendMessageW(GetDlgItem(g_hwnd, IDC_ENABLED), BM_GETCHECK, 0, 0) == BST_CHECKED;
  wchar_t buf[512];
  struct { int id; int* v; } ints[] = {
      {IDC_MIN_CODE, &g_p.min_code_len}, {IDC_MAX_CODE, &g_p.max_code_len},
      {IDC_MIN_TOK, &g_p.min_tokens},   {IDC_MAX_TOK, &g_p.max_tokens},
      {IDC_CORES, &g_p.cpu_cores},      {IDC_FREQ_K, &g_p.freq_k},
      {IDC_MAX_CAND, &g_p.max_candidates}};
  for (auto& r : ints) {
    GetDlgItemTextW(g_hwnd, r.id, buf, 64);
    *r.v = (int)wcstol(buf, NULL, 10);
  }
  struct { int id; double* v; } dbls[] = {{IDC_ELW, &g_p.elw},
                                          {IDC_FREQ_W, &g_p.freq_weight}};
  for (auto& r : dbls) {
    GetDlgItemTextW(g_hwnd, r.id, buf, 64);
    *r.v = wcstod(buf, NULL);
  }
  GetDlgItemTextW(g_hwnd, IDC_MODEL, buf, 512);
  std::wstring def = default_model_path();
  g_p.model_path = (wcscmp(buf, def.c_str()) == 0) ? std::wstring() : buf;
}

static void params_to_ui() {
  SendMessageW(GetDlgItem(g_hwnd, IDC_ENABLED), BM_SETCHECK,
               g_p.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  SetDlgItemTextW(g_hwnd, IDC_MODEL, shown_model().c_str());
  wchar_t buf[64];
  struct { int id; int v; const wchar_t* fmt; } rows[] = {
      {IDC_MIN_CODE, g_p.min_code_len, L"%d"}, {IDC_MAX_CODE, g_p.max_code_len, L"%d"},
      {IDC_MIN_TOK, g_p.min_tokens, L"%d"},    {IDC_MAX_TOK, g_p.max_tokens, L"%d"},
      {IDC_CORES, g_p.cpu_cores, L"%d"},       {IDC_ELW, 0, L"%.2f"},
      {IDC_FREQ_W, 0, L"%.2f"},                {IDC_FREQ_K, g_p.freq_k, L"%d"},
      {IDC_MAX_CAND, g_p.max_candidates, L"%d"}};
  for (auto& r : rows) {
    if (r.fmt[1] == L'd') swprintf_s(buf, L"%d", r.v);
    else if (r.id == IDC_ELW) swprintf_s(buf, L"%.2f", g_p.elw);
    else swprintf_s(buf, L"%.2f", g_p.freq_weight);
    SetDlgItemTextW(g_hwnd, r.id, buf);
  }
}

// ---- 控件创建 ----
static HWND mk(int cls, const wchar_t* text, DWORD style, int x, int y, int w,
               int h, int id) {
  static const wchar_t* C[] = {L"BUTTON", L"STATIC", L"EDIT"};
  HWND ctl = CreateWindowW(C[cls], text,
                           WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hwnd,
                           (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageW(ctl, WM_SETFONT, (WPARAM)g_font, TRUE);
  return ctl;
}

struct Lbl { const wchar_t* t; int x, y; };
// 布局铁律：任何控件的矩形不得与其他控件相交——不透明子控件按 z 序
// 覆盖先画者，会把被覆盖控件的文字"局部擦除"成叠字残片（2026-08-27
// 叠字事故根因：勾选框 w430 与下行标签矩形相交 + 空 DLSTATUS 静态框横贯
// 首行）。每行独占一个水平带，互不入侵。
static void make_ui() {
  g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
  mk(0, L"启用 LLM 重排（保存后立即生效，无需重新部署）",
     BS_AUTOCHECKBOX | WS_TABSTOP, 15, 12, 470, 22, IDC_ENABLED);
  // 第 2 行：模型路径（编辑框足够宽，默认完整路径含扩展名不再截尾）
  mk(1, L"模型路径:", 0, 15, 46, 68, 20, 0);
  mk(2, L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 86, 43, 356, 24,
     IDC_MODEL);
  mk(0, L"浏览…", WS_TABSTOP, 447, 42, 56, 25, IDC_BROWSE);
  mk(0, L"下载", WS_TABSTOP, 507, 42, 47, 25, IDC_DOWNLOAD);
  // 第 3 行：下载状态独立一行（此前横贯首行造成叠字）
  mk(1, L"", 0, 15, 74, 539, 18, IDC_DLSTATUS);
  // 参数两列（键名与 llm_rerank.yaml 相同，见下方说明）
  Lbl c1[] = {{L"最小编码长度", 15, 106},
              {L"最大编码长度（0=不限）", 15, 136},
              {L"最少上文 token", 15, 166},
              {L"上文 token 上限", 15, 196},
              {L"CPU 线程数", 15, 226}};
  int i1[] = {IDC_MIN_CODE, IDC_MAX_CODE, IDC_MIN_TOK, IDC_MAX_TOK, IDC_CORES};
  for (int i = 0; i < 5; i++) {
    mk(1, c1[i].t, 0, c1[i].x, c1[i].y, 168, 20, 0);
    mk(2, L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 186, c1[i].y - 3, 60, 22,
       i1[i]);
  }
  struct { const wchar_t* t; int y; int id; } col2[] = {
      {L"预期词长权重", 106, IDC_ELW}, {L"词频权重", 136, IDC_FREQ_W},
      {L"词频饱和常数", 166, IDC_FREQ_K}, {L"候选数上限", 196, IDC_MAX_CAND}};
  for (auto& r : col2) {
    mk(1, r.t, 0, 302, r.y, 126, 20, 0);
    mk(2, L"", WS_BORDER | WS_TABSTOP, 430, r.y - 3, 60, 22, r.id);
  }
  mk(1, L"说明：预期词长权重适用于两码一字方案（词长 = 码长/2 加分）",
     0, 15, 262, 539, 20, 0);
  mk(1, L"参数键名与 llm_rerank.yaml 相同；修改保存后立即生效",
     0, 15, 282, 539, 20, 0);
  mk(0, L"保存并生效", WS_TABSTOP | BS_DEFPUSHBUTTON, 15, 302, 110, 30,
     IDC_SAVE);
  mk(0, L"关闭", WS_TABSTOP, 133, 302, 70, 30, IDC_CLOSE);
  mk(1, L"", 0, 215, 308, 339, 18, IDC_STATUS);
  // llm_filter 为显式组件：GUI 只管参数，方案未列出则不参与重排（防"开了
  // 却没效果"的静默困惑）——窗口为此加高 26px，本行与状态行不相交
  mk(1, L"方案需在 engine/filters 列出 llm_filter 并重新部署才生效（详见 README）",
     0, 15, 336, 539, 18, 0);
}

static void on_browse() {
  wchar_t buf[512];
  GetDlgItemTextW(g_hwnd, IDC_MODEL, buf, 512);
  // 打开对话框对正斜杠初始路径报 FNERR_INVALIDFILENAME(0x3002) 静默失败——
  // 统一为反斜杠（显示/保存/yaml 随之一致，Windows API 两者都接受）
  for (wchar_t* c = buf; *c; ++c)
    if (*c == L'/') *c = L'\\';
  SetDlgItemTextW(g_hwnd, IDC_MODEL, buf);
  OPENFILENAMEW ofn = {sizeof(ofn)};
  ofn.hwndOwner = g_hwnd;
  ofn.lpstrFilter = L"GGUF 模型 (*.gguf)\0*.gguf\0所有文件 (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 512;
  // 选已有模型文件用打开对话框（原 Save 对话框选现有文件会弹覆盖确认）
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  if (GetOpenFileNameW(&ofn)) {
    SetDlgItemTextW(g_hwnd, IDC_MODEL, buf);
    return;
  }
  // 打开失败：状态行给出 CommDlg 错误码，便于用户机排查
  wchar_t msg[64];
  swprintf_s(msg, L"打开对话框失败（CommDlg 错误码 %lu）",
             CommDlgExtendedError());
  set_status(GetDlgItem(g_hwnd, IDC_STATUS), msg);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g_hwnd = h;
      make_ui();
      load_params();
      params_to_ui();
      // 首次提示：模型未下载 → 询问是否现在下载（用户定案 2026-08-27）
      {
        unsigned long long sz;
        if (!file_size(shown_model(), &sz)) {
          if (MessageBoxW(h,
                          L"模型尚未下载（约 500MB，ModelScope 断点续传，"
                          L"中断后重试自动续传）。\n\n是否现在下载？",
                          L"LLM 重排", MB_YESNO | MB_ICONQUESTION) == IDYES)
            start_download();
        }
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_BROWSE: on_browse(); return 0;
        case IDC_DOWNLOAD: start_download(); return 0;
        case IDC_SAVE: {
          ui_to_params();
          if (save_params())
            set_status(GetDlgItem(h, IDC_STATUS), L"已保存，立即生效");
          else
            set_status(GetDlgItem(h, IDC_STATUS), L"保存失败（无法写入用户目录）");
          return 0;
        }
        case IDC_CLOSE: DestroyWindow(h); return 0;
        case IDCANCEL:            // IsDialogMessage 把 ESC 映射为 IDCANCEL
          DestroyWindow(h);
          return 0;
      }
      break;
    case WM_TIMER:
      if (wp == 1) poll_download();
      return 0;
    case WM_DESTROY:
      if (g_curl) KillTimer(h, 1);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  // 主线程 STA：Vista+ 打开/保存对话框内部走 COM，缺初始化会静默失败
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  WNDCLASSW wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"WeaselLLMSetup";
  RegisterClassW(&wc);
  HWND h = CreateWindowExW(WS_EX_APPWINDOW, L"WeaselLLMSetup",
                           L"LLM 重排设置 — 小狼毫", WS_OVERLAPPEDWINDOW &
                               ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                           CW_USEDEFAULT, CW_USEDEFAULT, 585, 410, NULL, NULL,
                           inst, NULL);
  ShowWindow(h, show);
  UpdateWindow(h);
  MSG m;
  while (GetMessageW(&m, NULL, 0, 0) > 0) {
    if (!IsDialogMessageW(h, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  CoUninitialize();
  return 0;
}
